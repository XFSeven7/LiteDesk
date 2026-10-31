# LiteDesk

Windows 10/11 轻量桌面整理工具（C++ / Win32）。

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 自检

```powershell
cmake --build build --config Release --target LiteDeskTests
.\build\Release\LiteDeskTests.exe
```
