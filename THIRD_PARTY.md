# Third-party components

Sinan (司南, formerly TabTail) is licensed under MIT. Dependency licenses remain in force.

| Component | Version | License | Source |
| --- | --- | --- | --- |
| CLI11 | 2.5.0 | BSD-3-Clause | https://github.com/CLIUtils/CLI11 |
| nlohmann/json | 3.12.0 | MIT | https://github.com/nlohmann/json |
| toml++ | 3.4.0 | MIT | https://github.com/marzer/tomlplusplus |
| MinHook, including HDE64 | 1.3.4 | BSD-style; see LICENSE.txt for component notices | https://github.com/TsudaKageyu/minhook |
| FTXUI | 7.0.3 | MIT | https://github.com/ArthurSonzogni/FTXUI |
| Everything SDK3 | 3.0.1.12 | MIT | https://www.voidtools.com/forum/viewtopic.php?t=15853 |

CMake pins downloaded archives by SHA-256. The vendored toml++ header and its original license are in `third_party/tomlplusplus`. Binary packages include original dependency license files. Windows SDK components are provided by Microsoft under their own terms.

ExplorerPatcher, ExplorerTabUtility and ShellTabs were read as research references. Their local source trees under `refer/` are excluded from this repository; their licenses do not grant permission to relicense their code as MIT. The documented symbol/layout implementation is independent. Raw Explorer captures and screenshots containing user context are excluded as well.
