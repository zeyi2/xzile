# AX_REQUIRE_DEFINED(MACRO)
# -------------------------
# Allow to check if MACRO is defined.
AC_DEFUN([AX_REQUIRE_DEFINED], [dnl
  m4_ifndef([$1], [m4_fatal([macro ]$1[ is not defined; is a m4 file missing?])])
])
