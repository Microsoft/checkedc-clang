rem Create directories and sync files

set OLD_DIR=%CD%

if not exist %BUILD_SOURCESDIRECTORY%\llvm\.git (
  git clone -c core.autocrlf=false https://github.com/Microsoft/checkedc-llvm %BUILD_SOURCESDIRECTORY%\llvm
  if ERRORLEVEL 1 (goto cmdfailed)
)

if not exist %BUILD_SOURCESDIRECTORY%\llvm\tools\clang\.git (
  git clone -c core.autocrlf=false https://github.com/Microsoft/checkedc-clang %BUILD_SOURCESDIRECTORY%\llvm\tools\clang
  if ERRORLEVEL 1 (goto cmdfailed)
)

if not exist %BUILD_SOURCESDIRECTORY%\llvm\projects\checkedc-wrapper\checkedc\.git (
  git clone https://github.com/Microsoft/checkedc %BUILD_SOURCESDIRECTORY%\llvm\projects\checkedc-wrapper\checkedc
  if ERRORLEVEL 1 (goto cmdfailed)
)

rem set up LLVM sources
cd %BUILD_SOURCESDIRECTORY%\llvm
if ERRORLEVEL 1 (goto cmdfailed)
git fetch origin
if ERRORLEVEL 1 (goto cmdfailed)
git checkout -f %LLVM_BRANCH%
if ERRORLEVEL 1 (goto cmdfailed)
git pull -f origin %LLVM_BRANCH%
if ERRORLEVEL 1 (goto cmdfailed)

git checkout %LLVM_COMMIT%
if ERRORLEVEL 1 (goto cmdfailed)

rem set up Checked C sources
cd %BUILD_SOURCESDIRECTORY%\llvm\projects\checkedc-wrapper\checkedc
if ERRORLEVEL 1 (goto cmdfailed)
git fetch origin
if ERRORLEVEL 1 (goto cmdfailed)
git checkout -f %CHECKEDC_BRANCH%
if ERRORLEVEL 1 (goto cmdfailed)
git pull -f origin %CHECKEDC_BRANCH%
if ERRORLEVEL 1 (goto cmdfailed)

git checkout %CHECKEDC_COMMIT%
if ERRORLEVEL 1 (goto cmdfailed)

rem Set up clang sources
cd %BUILD_SOURCESDIRECTORY%\llvm\tools\clang
if ERRORLEVEL 1 (goto cmdfailed)
git fetch origin
if ERRORLEVEL 1 (goto cmdfailed)
git checkout -f %CHECKEDC_BRANCH%
if ERRORLEVEL 1 (goto cmdfailed)
git pull -f origin %CLANG_BRANCH%
if ERRORLEVEL 1 (goto cmdfailed)

git checkout %CLANG_COMMIT%
if ERRORLEVEL 1 (goto cmdfailed)

if not exist %LLVM_OBJ_DIR% (
  mkdir %LLVM_OBJ_DIR%
  if ERRORLEVEL 1 (goto cmdfailed)
)


:succeeded
  cd %OLD_DIR%
  exit /b 0

:cmdfailed
  echo.Setting up files failed
  cd %OLD_DIR%
  exit /b 1
