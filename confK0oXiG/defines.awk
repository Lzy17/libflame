BEGIN {
D["PACKAGE_NAME"]=" \"\""
D["PACKAGE_TARNAME"]=" \"\""
D["PACKAGE_VERSION"]=" \"\""
D["PACKAGE_STRING"]=" \"\""
D["PACKAGE_BUGREPORT"]=" \"\""
D["PACKAGE_URL"]=" \"\""
P["F77_FUNC"]="(name,NAME)"
D["F77_FUNC"]=" name ## _"
P["F77_FUNC_"]="(name,NAME)"
D["F77_FUNC_"]=" name ## _"
D["STDC_HEADERS"]=" 1"
D["HAVE_SYS_TYPES_H"]=" 1"
D["HAVE_SYS_STAT_H"]=" 1"
D["HAVE_STDLIB_H"]=" 1"
D["HAVE_STRING_H"]=" 1"
D["HAVE_MEMORY_H"]=" 1"
D["HAVE_STRINGS_H"]=" 1"
D["HAVE_INTTYPES_H"]=" 1"
D["HAVE_STDINT_H"]=" 1"
D["HAVE_UNISTD_H"]=" 1"
D["HAVE_FCNTL_H"]=" 1"
D["HAVE_STDLIB_H"]=" 1"
D["HAVE_STRING_H"]=" 1"
D["HAVE_SIGNAL_H"]=" 1"
D["HAVE_ASSERT_H"]=" 1"
D["HAVE_MATH_H"]=" 1"
D["HAVE_SYS_TIME_H"]=" 1"
D["TIME_WITH_SYS_TIME"]=" 1"
D["PROTOTYPES"]=" 1"
D["__PROTOTYPES"]=" 1"
D["_GNU_SOURCE"]=" 1"
D["FLA_ENABLE_NON_CRITICAL_CODE"]=" 1"
D["FLA_ENABLE_LAPACK2FLAME"]=" 1"
D["FLA_ENABLE_MULTITHREADING"]=" 1"
D["FLA_MULTITHREADING_MODEL"]=" 2"
D["FLA_ENABLE_SUPERMATRIX"]=" 1"
D["FLA_ENABLE_HIP"]=" 1"
D["FLA_VECTOR_INTRINSIC_TYPE"]=" 0"
D["FLA_ENABLE_INTERNAL_ERROR_CHECKING"]=" 1"
D["FLA_INTERNAL_ERROR_CHECKING_LEVEL"]=" 2"
D["FLA_ENABLE_BLIS1_USE_OF_FLA_MALLOC"]=" 1"
D["FLA_ENABLE_PORTABLE_TIMER"]=" 1"
D["FLA_PORTABLE_TIMER_IS_CLOCK_GETTIME"]=" 1"
D["HAVE_LIBM"]=" 1"
D["HAVE_LIBM"]=" 1"
D["HAVE_LIBM"]=" 1"
  for (key in D) D_is_set[key] = 1
  FS = ""
}
/^[\t ]*#[\t ]*(define|undef)[\t ]+[_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ][_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]*([\t (]|$)/ {
  line = $ 0
  split(line, arg, " ")
  if (arg[1] == "#") {
    defundef = arg[2]
    mac1 = arg[3]
  } else {
    defundef = substr(arg[1], 2)
    mac1 = arg[2]
  }
  split(mac1, mac2, "(") #)
  macro = mac2[1]
  prefix = substr(line, 1, index(line, defundef) - 1)
  if (D_is_set[macro]) {
    # Preserve the white space surrounding the "#".
    print prefix "define", macro P[macro] D[macro]
    next
  } else {
    # Replace #undef with comments.  This is necessary, for example,
    # in the case of _POSIX_SOURCE, which is predefined and required
    # on some systems where configure will not decide to define it.
    if (defundef == "undef") {
      print "/*", prefix defundef, macro, "*/"
      next
    }
  }
}
{ print }
