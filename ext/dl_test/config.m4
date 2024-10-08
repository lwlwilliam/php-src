PHP_ARG_ENABLE([dl-test],
  [whether to enable dl-test extension],
  [AS_HELP_STRING([--enable-dl-test],
    [Enable dl_test extension])])

if test "$PHP_DL_TEST" != "no"; then
  dnl Always build as shared extension.
  ext_shared=yes

  PHP_NEW_EXTENSION([dl_test],
    [dl_test.c],
    [$ext_shared],,
    [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])
fi
