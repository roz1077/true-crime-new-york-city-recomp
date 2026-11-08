# Keep the format in sync with runtime/app/game_code.cpp.
file(SHA1 "${HOST}" host_sha1)
file(SHA1 "${DLL}" dll_sha1)
file(WRITE "${DLL}.stamp.tmp" "${host_sha1} ${XBE_SHA1} ${dll_sha1}\n")
file(RENAME "${DLL}.stamp.tmp" "${DLL}.stamp")
