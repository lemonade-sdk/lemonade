#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <vector>

namespace nexus::boringtun {

/// A mesh Curve25519 keypair (base64-encoded strings).
struct BoringtunKeypair {
    std::string public_key;
    std::string private_key;
};

/// A mesh peer as reported by the BoringTun dataplane.
struct BoringtunPeer {
    std::string public_key;
    std::string allowed_ips;
    std::string endpoint;
    uint64_t    last_handshake{0};
    uint64_t    rx_bytes{0};
    uint64_t    tx_bytes{0};
    uint16_t    persistent_keepalive{25};
};

/// Configuration for a boringtun interface.
struct BoringtunInterfaceConfig {
    std::string private_key;
    std::string address;
    uint16_t    listen_port{51940};
    std::string dns;
};

/// A peer derived from a tree node, carrying the fields needed for mesh
/// configuration and sync operations.
struct TreeNodePeer {
    std::string public_key;       // mesh Curve25519 pubkey
    std::string tunnel_ip;        // e.g. "10.64.4.210/32"
    std::string private_subnet;   // e.g. "10.128.17.4/30"
    std::string endpoint;         // "ip:port" (may be empty)
    uint16_t    persistent_keepalive{25};
};

/// CRTP base for mesh operations.
/// Derived must implement:
///   BoringtunKeypair do_generate_keypair()
///   bool do_set_interface(const BoringtunInterfaceConfig& config)
///   bool do_add_peer(const std::string& pubkey, const std::string& allowed_ips, const std::string& endpoint)
///   bool do_remove_peer(const std::string& pubkey)
///   std::vector<BoringtunPeer> do_get_peers()
///   bool do_update_endpoint(const std::string& pubkey, const std::string& new_endpoint)
///   std::string do_generate_config(const BoringtunInterfaceConfig& config, const std::vector<BoringtunPeer>& peers)
///   bool do_setup_interface(const BoringtunInterfaceConfig& config, const std::vector<BoringtunPeer>& peers)
///   bool do_teardown_interface()
///   int do_sync_peers_from_tree(const std::vector<TreeNodePeer>& desired_peers)
///   bool do_save_config(const std::string& config_contents)
///   std::string do_load_config()
template <typename Derived>
class IBoringtunProvider {
public:
    [[nodiscard]] BoringtunKeypair generate_keypair() {
        return self().do_generate_keypair();
    }

    [[nodiscard]] bool set_interface(const BoringtunInterfaceConfig& config) {
        return self().do_set_interface(config);
    }

    [[nodiscard]] bool add_peer(const std::string& pubkey,
                                const std::string& allowed_ips,
                                const std::string& endpoint) {
        return self().do_add_peer(pubkey, allowed_ips, endpoint);
    }

    [[nodiscard]] bool remove_peer(const std::string& pubkey) {
        return self().do_remove_peer(pubkey);
    }

    [[nodiscard]] std::vector<BoringtunPeer> get_peers() {
        return self().do_get_peers();
    }

    [[nodiscard]] bool update_endpoint(const std::string& pubkey,
                                       const std::string& new_endpoint) {
        return self().do_update_endpoint(pubkey, new_endpoint);
    }

    /// Generate a wg-quick compatible config file string.
    [[nodiscard]] std::string generate_config(const BoringtunInterfaceConfig& config,
                                               const std::vector<BoringtunPeer>& peers) {
        return self().do_generate_config(config, peers);
    }

    /// Create the boringtun interface, assign address, set keys, bring up,
    /// add peers, and install routes.
    [[nodiscard]] bool setup_interface(const BoringtunInterfaceConfig& config,
                                       const std::vector<BoringtunPeer>& peers) {
        return self().do_setup_interface(config, peers);
    }

    /// Bring the interface down and delete it.
    [[nodiscard]] bool teardown_interface() {
        return self().do_teardown_interface();
    }

    /// Sync peers with the desired state from tree nodes.
    /// Returns the number of changes made (adds + removes + updates),
    /// or -1 on error.
    [[nodiscard]] int sync_peers_from_tree(const std::vector<TreeNodePeer>& desired_peers) {
        return self().do_sync_peers_from_tree(desired_peers);
    }

    /// Add a secondary address to the existing interface (no flush).
    /// Used to add the backbone address after setup_interface().
    [[nodiscard]] bool add_address(const std::string& address_cidr) {
        return self().do_add_address(address_cidr);
    }

    /// Persist the config string to disk.
    [[nodiscard]] bool save_config(const std::string& config_contents) {
        return self().do_save_config(config_contents);
    }

    /// Load the config string from disk. Returns empty string if not found.
    [[nodiscard]] std::string load_config() {
        return self().do_load_config();
    }

protected:
    ~IBoringtunProvider() = default;

private:
    [[nodiscard]] Derived& self() { return static_cast<Derived&>(*this); }
    [[nodiscard]] const Derived& self() const { return static_cast<const Derived&>(*this); }
};

/// Concept constraining a valid IBoringtunProvider implementation.
template <typename T>
concept BoringtunProviderType = requires(T t,
                                          const BoringtunInterfaceConfig& config,
                                          const std::string& s,
                                          const std::vector<BoringtunPeer>& peers,
                                          const std::vector<TreeNodePeer>& tree_peers) {
    { t.do_generate_keypair() } -> std::same_as<BoringtunKeypair>;
    { t.do_set_interface(config) } -> std::same_as<bool>;
    { t.do_add_peer(s, s, s) } -> std::same_as<bool>;
    { t.do_remove_peer(s) } -> std::same_as<bool>;
    { t.do_get_peers() } -> std::same_as<std::vector<BoringtunPeer>>;
    { t.do_update_endpoint(s, s) } -> std::same_as<bool>;
    { t.do_generate_config(config, peers) } -> std::same_as<std::string>;
    { t.do_setup_interface(config, peers) } -> std::same_as<bool>;
    { t.do_teardown_interface() } -> std::same_as<bool>;
    { t.do_sync_peers_from_tree(tree_peers) } -> std::same_as<int>;
    { t.do_add_address(s) } -> std::same_as<bool>;
    { t.do_save_config(s) } -> std::same_as<bool>;
    { t.do_load_config() } -> std::same_as<std::string>;
};

} // namespace nexus::boringtun
