/* Appended to Mbed TLS's default configuration (MBEDTLS_USER_CONFIG_FILE).
 *
 * libdatachannel's Mbed TLS DTLS transport calls
 * mbedtls_ssl_conf_dtls_srtp_protection_profiles() unconditionally --
 * src/impl/dtlstransport.cpp:416 in v0.24.5 -- and the surrounding code is
 * not guarded by RTC_ENABLE_MEDIA. So DTLS-SRTP has to be compiled into
 * Mbed TLS even for a build with NO_MEDIA=ON, which otherwise wants none of
 * it. Without this the build fails on undeclared MBEDTLS_TLS_SRTP_* .
 */
#define MBEDTLS_SSL_DTLS_SRTP
