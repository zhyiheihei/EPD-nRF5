# 开发环境：nix develop 进入后可完成 模拟器构建 / 固件交叉编译 / OTA 包生成
#
#   nix develop                 # 进入 devShell
#   make                        # 构建 GUI 模拟器（SDL2）
#   make -f Makefile.firmware   # 交叉编译 nRF52 固件
#   make -f Makefile.firmware ota  # 固件 + 蓝牙 OTA 包（ota.zip/full.hex/settings.hex）
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = inputs @ { flake-parts, nixpkgs, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [ "x86_64-linux" "aarch64-linux" ];

      perSystem = { system, ... }:
        let
          # nrfutil 6 的签名库 ecdsa 0.19.x 被 nixpkgs 标记 insecure（CVE-2024-23342，
          # 仅影响验证恶意签名，不影响本地自签），显式放行
          pkgs = import nixpkgs {
            inherit system;
            config.permittedInsecurePackages = [ "python3.11-ecdsa-0.19.2" ];
          };
          py = pkgs.python311;
          pp = py.pkgs;

          # nixpkgs 的 intelhex/tqdm 带有庞大的 check 依赖（matplotlib/numpy 等），
          # OTA 包生成用不到，这里用无测试的最小构建
          intelhexMinimal = pp.buildPythonPackage rec {
            pname = "intelhex";
            version = "2.3.0";
            pyproject = true;
            build-system = [ pp.setuptools ];
            src = pp.fetchPypi {
              inherit pname version;
              hash = "sha256-iStzYacZ9JRSN9qMz3VOlRPbMvViiFJ4WuoQjc0lAJM=";
            };
            doCheck = false;
          };

          tqdmMinimal = pp.buildPythonPackage rec {
            pname = "tqdm";
            version = "4.68.4";
            pyproject = true;
            build-system = [ pp.setuptools ];
            src = pp.fetchPypi {
              inherit pname version;
              hash = "sha256-GYKclnNjjyoLhhfaTNy5J+gxzYi8/LbnjUKk0a8TFSA=";
            };
            doCheck = false;
            # wheel 元数据会把 dev extras 里的 setuptools-scm 也算进 Requires-Dist
            propagatedBuildInputs = [ pp.setuptools-scm ];
          };

          # nrfutil 6.1.7 的 dfu_cc_pb2.py 是 proto2 时代生成的，只能跑在 protobuf <4 上；
          # nixpkgs 里只有 7.x，这里取 3.20.3 纯 Python wheel
          protobuf3_20 = pp.buildPythonPackage rec {
            pname = "protobuf";
            version = "3.20.3";
            format = "wheel";
            src = pp.fetchPypi {
              inherit pname version;
              format = "wheel";
              hash = "sha256-p8ptSIqo/38ynUxUWy262KwxRk8dixyHrRNGcXcx5Ns=";
            };
            doCheck = false;
            propagatedBuildInputs = [ pp.setuptools ];
          };

          # pc_ble_driver_py（nrfutil 的 BLE 串口传输后端）已从 PyPI 消失，
          # OTA 包生成只用到它的 exceptions 模块，做个带版本元数据的最小 stub
          pcBleDriverStub = pp.toPythonModule (pkgs.runCommand "pc_ble_driver_py-stub" { } ''
            mkdir -p $out/${py.sitePackages}/pc_ble_driver_py
            mkdir -p $out/${py.sitePackages}/pc_ble_driver_py-0.16.4.dist-info
            cat > $out/${py.sitePackages}/pc_ble_driver_py/__init__.py <<'EOF'
            __version__ = "0.16.4"
            EOF
            cat > $out/${py.sitePackages}/pc_ble_driver_py/exceptions.py <<'EOF'
            class NordicSemiException(Exception):
                pass


            class IllegalStateException(NordicSemiException):
                pass
            EOF
            cat > $out/${py.sitePackages}/pc_ble_driver_py/config.py <<'EOF'
            __conn_ic_id__ = "NRF52"
            EOF
            cat > $out/${py.sitePackages}/pc_ble_driver_py-0.16.4.dist-info/METADATA <<'EOF'
            Metadata-Version: 2.1
            Name: pc_ble_driver_py
            Version: 0.16.4
            EOF
            cat > $out/${py.sitePackages}/pc_ble_driver_py-0.16.4.dist-info/WHEEL <<'EOF'
            Wheel-Version: 1.0
            EOF
          '');

          # nrfutil 依赖的 pyspinel 不在 nixpkgs；它只用到了 spinel.util.hex_to_bytes
          spinelStub = pp.toPythonModule (pkgs.runCommand "pyspinel-stub" { } ''
            mkdir -p $out/${py.sitePackages}/spinel
            mkdir -p $out/${py.sitePackages}/pyspinel-1.0.0a3.dist-info
            : > $out/${py.sitePackages}/spinel/__init__.py
            cat > $out/${py.sitePackages}/spinel/util.py <<'EOF'
            def hex_to_bytes(s):
                return bytes.fromhex(s)
            EOF
            cat > $out/${py.sitePackages}/pyspinel-1.0.0a3.dist-info/METADATA <<'EOF'
            Metadata-Version: 2.1
            Name: pyspinel
            Version: 1.0.0a3
            EOF
            cat > $out/${py.sitePackages}/pyspinel-1.0.0a3.dist-info/WHEEL <<'EOF'
            Wheel-Version: 1.0
            EOF
          '');

          # nrfutil 元数据里剩余的运行时依赖（仅 BLE/串口传输、thread/zigbee 烧录用到），
          # 用带版本元数据的空 stub 满足 pythonRuntimeDepsCheck
          nrfutilDepsStub = pp.toPythonModule (pkgs.runCommand "nrfutil-deps-stub" { } ''
            for name_ver in libusb1-1.9.3 piccata-1.0.0 antlib-1.1b0; do
              name=''${name_ver%-*}
              mkdir -p $out/${py.sitePackages}/$name_ver.dist-info
              printf 'Metadata-Version: 2.1\nName: %s\nVersion: %s\n' "$name" "''${name_ver#*-}" \
                > $out/${py.sitePackages}/$name_ver.dist-info/METADATA
              printf 'Wheel-Version: 1.0\n' > $out/${py.sitePackages}/$name_ver.dist-info/WHEEL
            done
          '');

          # legacy DFU 包生成必须用 nrfutil 6.x（nixpkgs 的 nrfutil 8.x 已移除该功能）
          nrfutil6 = pp.buildPythonApplication rec {
            pname = "nrfutil";
            version = "6.1.7";
            pyproject = true;
            build-system = [ pp.setuptools ];
            src = pp.fetchPypi {
              inherit pname version;
              hash = "sha256-zetRVc1l72LBlmQs+rmiq9ZfxhzndDmxpFSfaL/C/7Y=";
            };
            doCheck = false;
            propagatedBuildInputs = with pp; [
              click
              crcmod
              ecdsa
              pyserial
              pyyaml
              protobuf3_20
              intelhexMinimal
              tqdmMinimal
              pcBleDriverStub
              spinelStub
              nrfutilDepsStub
            ];
            pythonImportsCheck = [ "nordicsemi.__main__" ];
          };
        in
        {
          packages = {
            nrfutil6 = nrfutil6;
            default = nrfutil6;
          };

          devShells.default = pkgs.mkShell {
            name = "EPD-nRF5";
            packages = [
              pkgs.gcc
              pkgs.gnumake
              pkgs.pkg-config
              pkgs.sdl2-compat # SDL2 接口层，pkg-config 名仍为 sdl2
              pkgs.gcc-arm-embedded-13
              (py.withPackages (ps: [ intelhexMinimal ])) # tools/mergehex 依赖
              nrfutil6
            ];
          };
        };
    };
}
