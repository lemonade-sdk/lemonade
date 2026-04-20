{
  description = "Nix flake packaging for Lemonade server";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = {
    self,
    nixpkgs,
  }:
  let
    version = "10.2.0";

    systems = [
      "x86_64-linux"
      "aarch64-linux"
    ];

    eachSystem = f:
      nixpkgs.lib.genAttrs systems (
        system: f system
      );
  in
  {
    packages = eachSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      let
        lemonade-server = pkgs.stdenv.mkDerivation {
          pname = "lemonade-server";
          inherit version;

          src = ./.;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
          ];

          buildInputs = with pkgs; [
            cli11
            httplib
            curl
            libdrm
            libcap
            libwebsockets
            nlohmann_json
            systemd
            zstd
          ];

          postPatch = ''
            substituteInPlace CMakeLists.txt \
              --replace-fail 'find_path(HTTPLIB_INCLUDE_DIRS "httplib.h")' 'find_path(HTTPLIB_INCLUDE_DIRS "httplib.h")
find_package(httplib CONFIG QUIET)
if(TARGET httplib::httplib AND NOT TARGET cpp-httplib)
    add_library(cpp-httplib ALIAS httplib::httplib)
endif()
if(TARGET httplib::httplib)
    set(HTTPLIB_FOUND ON)
endif()'

            # Nix packaging: avoid host /usr symlink creation during install.
            substituteInPlace CMakeLists.txt \
              --replace-fail 'if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")' 'if(FALSE)'
            substituteInPlace src/cpp/cli/CMakeLists.txt \
              --replace-fail 'if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")' 'if(FALSE)'
            substituteInPlace src/cpp/legacy-cli/CMakeLists.txt \
              --replace-fail 'if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")' 'if(FALSE)'
            substituteInPlace src/cpp/tray/CMakeLists.txt \
              --replace-fail 'if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")' 'if(FALSE)'

            # Keep configuration files in the Nix output instead of trying to install to /etc.
            substituteInPlace CMakeLists.txt \
              --replace-fail 'DESTINATION /etc/lemonade/conf.d' 'DESTINATION etc/lemonade/conf.d'
          '';

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_WEB_APP=OFF"
            "-DUSE_SYSTEM_NODEJS_MODULES=ON"
          ];

          # The upstream CMake scripts install distro-specific files (e.g. /etc snippets,
          # /usr symlink helpers). Keep the Nix output clean and self-contained.
          postInstall = ''
            rm -rf "$out/etc"
            rm -rf "$out/usr"

            # lemond resolves defaults.json via <exe-dir>/resources/defaults.json
            # so provide bin/resources in Nix output as a symlink.
            ln -sfn ../share/lemonade-server/resources "$out/bin/resources"
          '';

          meta = with pkgs.lib; {
            description = "Local LLM server with OpenAI/Ollama/Anthropic-compatible APIs";
            homepage = "https://github.com/lemonade-sdk/lemonade";
            license = licenses.asl20;
            platforms = platforms.linux;
            mainProgram = "lemond";
          };
        };

        lemonade-tools = pkgs.runCommand "lemonade-tools-${version}" { } ''
          mkdir -p "$out/bin"
          ln -s ${lemonade-server}/bin/lemonade "$out/bin/lemonade"
          ln -s ${lemonade-server}/bin/lemonade-server "$out/bin/lemonade-server"
        '';
      in
      {
        inherit lemonade-server lemonade-tools;

        default = self.packages.${system}.lemonade-server;
      }
    );

    apps = eachSystem (
      system:
      {
        lemond = {
          type = "app";
          program = "${self.packages.${system}.lemonade-server}/bin/lemond";
        };

        lemonade = {
          type = "app";
          program = "${self.packages.${system}.lemonade-server}/bin/lemonade";
        };

        lemonade-server = {
          type = "app";
          program = "${self.packages.${system}.lemonade-server}/bin/lemonade-server";
        };

        default = {
          type = "app";
          program = "${self.apps.${system}.lemond.program}";
        };
      }
    );

    nixosModules.default =
      {
        config,
        lib,
        pkgs,
        ...
      }:
      let
        cfg = config.lemonade-server;
        execStart = lib.concatStringsSep " " (
          map lib.escapeShellArg (
            [
              "${cfg.package}/bin/lemond"
              "--host"
              cfg.host
              "--port"
              (toString cfg.port)
              cfg.stateDir
            ]
            ++ cfg.extraArgs
          )
        );
      in
      {
        options.lemonade-server = {
          enable = lib.mkEnableOption "Lemonade local LLM server";

          package = lib.mkOption {
            type = lib.types.package;
            default = self.packages.${pkgs.stdenv.hostPlatform.system}.lemonade-server;
            description = "Package providing the lemond, lemonade, and lemonade-server executables.";
          };

          toolsPackage = lib.mkOption {
            type = lib.types.package;
            default = self.packages.${pkgs.stdenv.hostPlatform.system}.lemonade-tools;
            description = "Package added to systemPackages (exports lemonade and lemonade-server only).";
          };

          user = lib.mkOption {
            type = lib.types.str;
            default = "lemonade";
            description = "User account that runs the Lemonade service.";
          };

          group = lib.mkOption {
            type = lib.types.str;
            default = "lemonade";
            description = "Group that runs the Lemonade service.";
          };

          host = lib.mkOption {
            type = lib.types.str;
            default = "127.0.0.1";
            description = "Host/interface for lemond to bind.";
          };

          port = lib.mkOption {
            type = lib.types.port;
            default = 13305;
            description = "TCP port for lemond.";
          };

          stateDir = lib.mkOption {
            type = lib.types.str;
            default = "/var/lib/lemonade";
            description = "Persistent state directory passed as the lemond cache_dir argument.";
          };

          environmentFiles = lib.mkOption {
            type = lib.types.listOf lib.types.str;
            default = [ "-/etc/lemonade/conf.d/*.conf" ];
            description = "EnvironmentFile entries for secrets such as HF_TOKEN and LEMONADE_API_KEY.";
          };

          environment = lib.mkOption {
            type = lib.types.attrsOf lib.types.str;
            default = { };
            description = "Extra environment variables passed to the service.";
          };

          extraArgs = lib.mkOption {
            type = lib.types.listOf lib.types.str;
            default = [ ];
            description = "Additional command line arguments passed to lemond.";
          };

          openFirewall = lib.mkOption {
            type = lib.types.bool;
            default = false;
            description = "Whether to open the Lemonade port in the firewall.";
          };

          useFHSEmulation = lib.mkOption {
            type = lib.types.bool;
            default = false;
            description = "Enable nix-ld + envfs compatibility so Lemonade can run downloaded Ubuntu prebuilt backends (for example llama.cpp Vulkan) on NixOS without rebuilding them from source. Leave disabled when using fully Nix-native backend binaries.";
          };

          fhs = lib.mkOption {
            type = lib.types.submodule {
              options = {
                llamaTargets = lib.mkOption {
                  type = lib.types.listOf (lib.types.enum [ "vulkan" "rocm" "cuda" "npu" ]);
                  default = [ "vulkan" ];
                  description = "Selected llama.cpp runtime targets used to derive nix-ld libraries.";
                };

                baseLibraries = lib.mkOption {
                  type = lib.types.listOf lib.types.package;
                  default = with pkgs; [
                    stdenv.cc.cc.lib
                    zlib
                    zstd
                  ];
                  description = "Base nix-ld runtime libraries used for every selected llama.cpp target.";
                };

                targetLibraries = lib.mkOption {
                  type = lib.types.attrsOf (lib.types.listOf lib.types.package);
                  default = {
                    vulkan = with pkgs; [
                      vulkan-loader
                      libglvnd
                    ];
                    rocm = with pkgs; [
                      rocmPackages.clr               # Common Language Runtime for ROCm
                      rocmPackages.rocblas           # ROCm BLAS library
                      rocmPackages.hipblas
                    ];
                    cuda = [
                      cudaPackages.libcublas
                      cudaPackages.libcurand
                      cudaPackages.cuda_cudart
                    ];
                    npu = [ ];
                  };
                  description = "Per-target nix-ld runtime libraries keyed by llama.cpp backend target.";
                };
              };
            };
            default = { };
            description = "FHS emulation settings used to derive nix-ld libraries.";
          };
        };

        config = lib.mkIf cfg.enable {
          users.groups = lib.mkIf (cfg.group == "lemonade") {
            lemonade = { };
          };

          users.users = lib.mkIf (cfg.user == "lemonade") {
            lemonade = {
              isSystemUser = true;
              group = cfg.group;
              home = cfg.stateDir;
              createHome = true;
              description = "Lemonade service user";
            };
          };

          systemd.tmpfiles.rules = [
            "d ${cfg.stateDir} 0750 ${cfg.user} ${cfg.group} -"
          ];

          environment.systemPackages = [
            cfg.toolsPackage
          ];

          networking.firewall.allowedTCPPorts = lib.mkIf cfg.openFirewall [ cfg.port ];

          warnings =
            lib.optionals (cfg.useFHSEmulation && cfg.fhs.llamaTargets == [ ]) [
              "lemonade-server.useFHSEmulation is enabled but no llama targets were selected; nix-ld will only use the base runtime libraries."
            ]
            ++ lib.optionals (lib.elem "cuda" cfg.fhs.llamaTargets) [
              "lemonade-server.fhs.llamaTargets includes cuda. Lemonade currently does not expose a llama.cpp cuda backend, so this only affects nix-ld library selection."
            ]
            ++ lib.optionals (lib.elem "npu" cfg.fhs.llamaTargets) [
              "lemonade-server.fhs.llamaTargets includes npu. Lemonade currently does not expose a llama.cpp npu backend, so this only affects nix-ld library selection."
            ];

          assertions = [
            {
              assertion = builtins.length cfg.fhs.llamaTargets == builtins.length (lib.unique cfg.fhs.llamaTargets);
              message = "lemonade-server.fhs.llamaTargets must not contain duplicate entries.";
            }
          ];

          programs.nix-ld = lib.mkIf (cfg.useFHSEmulation || cfg.fhs.llamaTargets != [ ]) {
            enable = true;
            libraries =
              let
                selectedTargetLibraries = lib.concatLists (
                  map (target: cfg.fhs.targetLibraries.${target} or [ ]) cfg.fhs.llamaTargets
                );
              in
              lib.unique (cfg.fhs.baseLibraries ++ selectedTargetLibraries);
          };

          systemd.services.lemonade-server = {
            description = "Lemonade Server";
            after = [ "network-online.target" ];
            wants = [ "network-online.target" ];
            wantedBy = [ "multi-user.target" ];

            # Backend installers call shell tools via system() (tar/unzip/which).
            # Add them explicitly to the unit PATH on NixOS.
            path = with pkgs; [
              gnutar
              gzip
              unzip
              which
            ];

            serviceConfig = {
              Type = "simple";
              User = cfg.user;
              Group = cfg.group;
              WorkingDirectory = cfg.stateDir;
              EnvironmentFile = cfg.environmentFiles;
              ExecStart = execStart;
              Restart = "on-failure";
              RestartSec = "5s";
              KillSignal = "SIGINT";
              AmbientCapabilities = [ "CAP_SYS_RESOURCE" ];

              PrivateTmp = true;
              NoNewPrivileges = true;
              ProtectSystem = "full";
              ProtectHome = true;
              ReadWritePaths = [ cfg.stateDir ];
              RestrictRealtime = true;
              RestrictNamespaces = true;
              LockPersonality = true;
            };

            environment = cfg.environment;
          };
        };
      };
  };
}
