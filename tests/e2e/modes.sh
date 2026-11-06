if command -v icacls >/dev/null 2>&1 && command -v cygpath >/dev/null 2>&1; then
  private_file() { MSYS2_ARG_CONV_EXCL='*' icacls "$(cygpath -w "$1")" /inheritance:r /remove "*S-1-1-0" /grant:r "${USERNAME}:F" > /dev/null; }
  open_file() { MSYS2_ARG_CONV_EXCL='*' icacls "$(cygpath -w "$1")" /grant '*S-1-1-0:R' > /dev/null; }
else
  private_file() { chmod 600 "$1"; }
  open_file() { chmod 644 "$1"; }
fi
