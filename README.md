cpm — 简易 C++ 包管理器 (实验性)

简介
----
cpm 是一个小型的本地 C++ 包管理器，支持包元数据自动生成、零拷贝本地安装、打包与本地注册。主要用于管理使用模块化 C++（modules / header units / module interface files）的包。

快速开始
--------
1. 构建二进制（在项目根）：
   cmake -S . -B build
   cmake --build build --config Release

2. 初始化本地 registry:
   cpm init

常用命令
-------
- cpm init
  创建本地 registry（~/.cppm/registry 或 %USERPROFILE%\.cppm\registry）

- cpm list
  列出 registry 中已注册的包

- cpm search <term>
  按名称模糊/精确搜索包

- cpm autogen [path]
  为目录自动生成 cppmod.toml（从 pyproject.toml、CMakeLists.txt、README 提取信息），会净化包名（非法字符替为下划线）并检测依赖候选。

- cpm install <path|name@version>
  安装本地目录或 registry 中的包。对本地目录优先尝试零拷贝移动（rename），若失败回退为复制；若目录已存在但未登记，会自动注册索引。
  注意：若要移动当前目录，请从目录的父目录运行 install 或使用绝对路径（避免占用导致移动失败）。

- cpm pack <path|name@version>
  打包目录或 registry 中的包为 name@version.tar.gz。支持 .cppmignore 来排除不需要的文件/目录（语法与 .gitignore 类似，当前实现将每行作为 tar --exclude 模式）。

- cpm publish <archive.tar.gz>
  将包发布到本地 registry（把 archive 拷贝到 registry/packages 并更新索引）

- cpm test <name@version> [--force-fallback]
  在临时目录用 CMake 测试包的集成构建；若 CMake 不可用或 --force-fallback 指定，会尝试直接用 g++/clang++/icx 回退编译（模块支持受编译器版本和选项影响）。

.cppmignore
-----------
放在包根的 .cppmignore 用于在 pack 时指定要排除的路径/模式。工具内置了一些默认忽略模式（build/, cmake-build-*/, .git/, 二进制/模块输出等）。

注意事项与提示
----------------
- Windows 上要注意路径与当前工作目录：如果要移动目录进行零拷贝安装，请不要在该目录内运行 cpm install（会因占用失败）。
- autogen 会把非法包名字符替换为下划线（例如 thread-pool → thread_pool），并会使用绝对路径的目录名作为回退名，避免生成空名（".")。
- 模块构建在不同编译器间有差异。MSVC+CMake 路径是最稳定的；要用 g++/clang++ 支持模块，请确保已安装支持模块的编译器版本并在 PATH 中。

开发与贡献
-------------
源码位于本仓库。若要修改并测试：
  cmake -S . -B build
  cmake --build build --config Debug

作者
----
实验性工具。若需更多自动化或 CI 支持，可提交 issue 或 PR。