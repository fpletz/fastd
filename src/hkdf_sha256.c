/*
  Copyright (c) 2012-2013, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "hkdf_sha256.h"

#include <string.h>


void fastd_hkdf_sha256_expand(fastd_sha256_t *out, size_t blocks, fastd_sha256_t *prk, const uint8_t *info, size_t infolen) {
	if (!blocks)
		return;

	size_t len = sizeof(fastd_sha256_t) + infolen + 1;

	uint8_t buf[len] __attribute__((align(4)));

	memset(buf, 0, sizeof(fastd_sha256_t));
	memcpy(buf+sizeof(fastd_sha256_t), info, infolen);
	buf[len-1] = 0x01;

	fastd_hmacsha256(out, prk->w, (uint32_t*)(buf+sizeof(fastd_sha256_t)), infolen + 1);

	while (--blocks) {
		memcpy(buf, out, sizeof(fastd_sha256_t));
		out++;
		buf[len-1]++;

		fastd_hmacsha256(out, prk->w, (uint32_t*)buf, len);
	}
}
