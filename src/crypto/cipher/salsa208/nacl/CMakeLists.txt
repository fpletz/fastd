fastd_cipher_impl(salsa208 nacl
  salsa208_nacl.c
)
fastd_cipher_impl_include_directories(salsa208 nacl ${NACL_INCLUDE_DIRS})
fastd_cipher_impl_link_libraries(salsa208 nacl ${NACL_LIBRARIES})
fastd_cipher_impl_require(salsa208 nacl NACL)
