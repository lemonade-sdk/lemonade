#pragma once

// The model-agnostic half of amica-engine: a D3D12 device, the AMICA
// compiler and execution objects, and the graphs compiled with them.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <amica/amica.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amica_engine {

using Microsoft::WRL::ComPtr;

// Upper bounds for one command list, as the AMICA API recommends, so a long
// graph never runs as a single submission long enough to trip the GPU timeout.
constexpr std::size_t kMaxDispatchesPerList = 32;
constexpr std::uint64_t kMaxWorkgroupsPerList = 262144;

// Weights placed in VRAM are staged through an upload buffer this large, so a
// model far larger than host memory never needs a second full copy.
constexpr std::uint64_t kUploadChunkBytes = 256ull << 20;

// The largest system-memory weight allocated as one buffer. Some drivers remove
// the device when a single GPU-visible system allocation reaches 4 GiB, so larger
// weights are stitched from heaps of this size behind one reserved address range.
constexpr std::uint64_t kSystemHeapBytes = 2ull << 30;

// D3D12 maps reserved buffers in 64 KiB tiles.
constexpr std::uint64_t kTileBytes = 64ull << 10;

void log(const std::string& message);
double milliseconds_since(std::chrono::steady_clock::time_point start);
std::string gib(std::uint64_t bytes);
void check_hr(HRESULT hr, const char* what);
std::wstring widen(const std::string& text);
std::string narrow(const wchar_t* text);
float half_to_float(std::uint16_t h);
std::uint16_t float_to_half(float value);
std::size_t element_bytes(std::int32_t element_type);

// The GPU architecture AMICA compiles for, from the adapter's PCI device ID.
const char* gfx_arch_of(UINT pci_device_id);

// A file mapped read-only. compile_graph borrows its resources until the graph's
// final release, so mappings live as long as the engine.
class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        file_ = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot open " + path);
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(file_, &size);
        size_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        data_ = mapping_ != nullptr ? MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (data_ == nullptr) {
            throw std::runtime_error("cannot map " + path);
        }
    }
    ~MappedFile() {
        if (data_ != nullptr) UnmapViewOfFile(data_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(data_); }
    std::size_t size() const { return size_; }
    std::string_view text() const { return {reinterpret_cast<const char*>(data()), size_}; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const void* data_ = nullptr;
    std::size_t size_ = 0;
};

struct Tensor {
    std::string dtype;
    std::vector<std::uint64_t> shape;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

class SafeTensors {
public:
    explicit SafeTensors(const MappedFile& file);

    const Tensor& at(const std::string& name) const;

private:
    std::map<std::string, Tensor> tensors_;
};

// A safetensors file kept mapped for the engine's lifetime: compile_graph
// borrows it, and external weights are uploaded from it by name.
struct Resources {
    explicit Resources(const std::string& path) : file(path), tensors(file) {}
    MappedFile file;
    SafeTensors tensors;
};

// A D3D12 device with one compute queue. Work is recorded into a pool of
// command lists and submitted in order with a single wait at the end.
class Gpu {
public:
    Gpu(const std::string& adapter_selector, const std::string& arch_override);
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    ID3D12Device* device() const { return device_.Get(); }
    const std::string& adapter_name() const { return adapter_name_; }
    const std::string& gfx_arch() const { return gfx_arch_; }
    bool uma() const { return uma_; }

    // The video memory the OS currently lets this process use: local memory
    // on a discrete GPU.
    std::uint64_t local_memory_budget() const;

    ComPtr<ID3D12Resource> create_buffer(std::uint64_t size, D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT);

    // A buffer in system memory that the CPU writes directly and kernels read
    // in place, over PCIe on a discrete GPU.
    ComPtr<ID3D12Resource> create_system_buffer(std::uint64_t size);

    // See kSystemHeapBytes. The heaps must outlive the returned buffer.
    ComPtr<ID3D12Resource> create_tiled_system_buffer(std::uint64_t size, const std::uint8_t* data,
                                                      std::vector<ComPtr<ID3D12Heap>>& heaps);

    // Starts a new command list that orders after everything recorded before it.
    ID3D12GraphicsCommandList* begin_list();
    ID3D12GraphicsCommandList* list() const { return current_; }

    void uav_barrier();
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    // Copies into a default-heap buffer that otherwise stays in the
    // unordered-access state AMICA requires.
    void copy_into(ID3D12Resource* destination, std::uint64_t destination_offset, ID3D12Resource* source,
                   std::uint64_t source_offset, std::uint64_t size);
    void copy_between(ID3D12Resource* destination, std::uint64_t destination_offset, ID3D12Resource* source,
                      std::uint64_t source_offset, std::uint64_t size);
    void read_back(ID3D12Resource* readback, ID3D12Resource* source, std::uint64_t offset, std::uint64_t size);

    // Executes every list recorded since the last submit, in order, and waits.
    void submit_and_wait();

private:
    ComPtr<ID3D12Resource> create(std::uint64_t size, const D3D12_HEAP_PROPERTIES& properties,
                                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);

    struct Recording {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
    };

    ComPtr<IDXGIAdapter3> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    std::uint64_t fence_value_ = 0;
    std::vector<Recording> lists_;
    std::size_t used_ = 0;
    ID3D12GraphicsCommandList* current_ = nullptr;
    std::string adapter_name_;
    std::string gfx_arch_;
    bool uma_ = false;
};

// The AMICA objects, with failures turned into exceptions that carry the
// message AMICA's error callback received.
class Amica {
public:
    Amica();
    ~Amica();
    Amica(const Amica&) = delete;
    Amica& operator=(const Amica&) = delete;

    void check(amica_status status, const char* what);

    amica::interface_ptr<amica_factory> factory;
    amica::interface_ptr<amica_diagnostics> diagnostics;
    amica::interface_ptr<amica_compiler> compiler;
    amica::interface_ptr<amica_execution> execution;

private:
    static void AMICA_CALL on_error(void* user_data, amica_status, const char*, const char* message);

    std::string last_error_;
};

// One compiled graph with its PSO list, persistent buffer and the GPU
// addresses every encode passes.
struct Graph {
    std::string label;
    Resources* resources = nullptr;  // null for a graph without weights
    amica::interface_ptr<amica_graph_interface> graph;
    amica::interface_ptr<amica_pso_list> psos;
    amica::buffer_requirements requirements;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;  // first kernel, count
    ComPtr<ID3D12Resource> persistent;
    std::vector<ComPtr<ID3D12Resource>> input_buffers;
    std::vector<ComPtr<ID3D12Resource>> output_buffers;
    std::vector<std::uint64_t> inputs;
    std::vector<std::uint64_t> outputs;
    std::vector<std::uint64_t> weights;

    int input_index(const std::string& name) const {
        if (name.empty()) return -1;
        for (std::uint32_t i = 0; i < requirements.num_inputs; ++i) {
            if (name == requirements.inputs[i].name) return static_cast<int>(i);
        }
        return -1;
    }

    const amica_io_desc& input(int index) const { return requirements.inputs[index]; }
    const amica_io_desc& output(int index) const { return requirements.outputs[index]; }

    void bind_input(int index, const ComPtr<ID3D12Resource>& buffer) {
        input_buffers[index] = buffer;
        inputs[index] = buffer->GetGPUVirtualAddress();
    }

    void bind_output(int index, const ComPtr<ID3D12Resource>& buffer) {
        output_buffers[index] = buffer;
        outputs[index] = buffer->GetGPUVirtualAddress();
    }
};

// A default-heap input with a persistently mapped upload twin. Each request
// writes the twin on the CPU and records a copy into the input.
struct HostInput {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12Resource> upload;
    std::uint8_t* mapped = nullptr;
    std::uint64_t size = 0;
    std::size_t width = 0;  // bytes per integer element; 0 for floating-point inputs

    explicit operator bool() const { return static_cast<bool>(buffer); }
    std::size_t elements() const { return width == 0 ? 0 : static_cast<std::size_t>(size / width); }

    void write(std::size_t index, std::int64_t value);

    void fill(std::int64_t value) {
        for (std::size_t i = 0; i < elements(); ++i) write(i, value);
    }
};

// The device, AMICA and everything the graphs compiled on them share: the PSO
// cache, the mapped weight files, the uploaded external weights and the scratch
// buffers. Graphs run one after another on the one queue, never concurrently.
class Runtime {
public:
    Runtime(const std::string& adapter, const std::string& arch);

    Gpu& gpu() { return gpu_; }
    const Gpu& gpu() const { return gpu_; }

    Resources& open_resources(const std::string& path);

    // options.device_info is set to this device. resources may be null.
    Graph compile(const std::string& label, std::string_view ir, Resources* resources,
                  amica::compile_options options);

    // Every graph names its external weights by tensor; a tensor two graphs
    // read from the same file is uploaded once and bound to both. Weights fill
    // VRAM smallest first up to the budget (unlimited when negative), and the
    // rest are placed in system memory.
    void upload_weights(const std::vector<Graph*>& graphs, std::int64_t vram_budget_mib);
    std::uint64_t weights_vram_bytes() const { return weights_vram_bytes_; }

    HostInput host_input(const amica_io_desc& io, bool integer);
    void upload(HostInput& input);

    // Creates the scratch buffers, sized for the largest graph, then the
    // persistent buffers, and runs each graph's init_graph.
    void initialize_graphs(const std::vector<Graph*>& graphs);

    void encode(Graph& graph);

    bool device_removed();

private:
    Gpu gpu_;
    std::unique_ptr<Amica> amica_;
    amica::interface_ptr<amica_device> device_;
    amica::interface_ptr<amica_pso_cache> pso_cache_;
    std::map<std::string, std::unique_ptr<Resources>> resources_;
    std::vector<ComPtr<ID3D12Resource>> external_weights_;
    std::vector<ComPtr<ID3D12Heap>> system_heaps_;
    std::uint64_t weights_vram_bytes_ = 0;
    std::uint64_t weights_system_bytes_ = 0;
    ComPtr<ID3D12Resource> scratch_;
    ComPtr<ID3D12Resource> init_scratch_;
};

}  // namespace amica_engine
