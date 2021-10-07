#include "ubiq/platform.h"

#include <system_error>
#include <cstring>

using namespace ubiq::platform::fpe;


encryption::encryption(const credentials & creds)
{
    struct ubiq_platform_fpe_enc_dec_obj * enc(nullptr);
    int res;

    res = ubiq_platform_fpe_enc_dec_create(&*creds, &enc);
    if (res != 0) {
        throw std::system_error(-res, std::generic_category(), get_error(enc));
    }

    _enc.reset(enc, &ubiq_platform_fpe_enc_dec_destroy);
}

std::string
encryption::encrypt(
  const std::string & ffs_name,
  const std::string & pt
)
{
  return encrypt(ffs_name, std::vector<std::uint8_t>(), pt);
}

std::string
encryption::encrypt(
  const std::string & ffs_name,
  const std::vector<std::uint8_t> & tweak,
  const std::string & pt
)
{
  std::string ct;
  char * ctbuf;
  size_t ctlen;
  int res;

  res = ubiq_platform_fpe_encrypt_data(
    _enc.get(), ffs_name.data(),
    tweak.data(), tweak.size(),
    pt.data(), pt.length(),
    &ctbuf, &ctlen);
  if (res != 0) {
      throw std::system_error(-res, std::generic_category(), get_error(_enc.get()));
  }

  ct = std::string(ctbuf, ctlen);
  std::free(ctbuf);
  return ct;
}


std::string
ubiq::platform::fpe::encrypt(
    const credentials & creds,
    const std::string & ffs_name,
    const std::vector<std::uint8_t> & tweak,
    const std::string & pt)
{
    std::string v;
    char * ctbuf;
    size_t ctlen;
    int res;

    res = ubiq_platform_fpe_encrypt(&*creds, ffs_name.data(),
    tweak.data(), tweak.size(),
    pt.data(), pt.length(),
    &ctbuf, &ctlen);
    if (res != 0) {
        throw std::system_error(-res, std::generic_category());
    }

    v = std::string(ctbuf, ctlen);
    // v.resize(ctlen);
    // std::memcpy(v.data(), ctbuf, ctlen);
    std::free(ctbuf);

    return v;
}

std::string
ubiq::platform::fpe::encrypt(
    const credentials & creds,
    const std::string & ffs_name,
    const std::string & pt)
{
  return encrypt(creds, ffs_name, std::vector<std::uint8_t>(), pt);
}

std::string
ubiq::platform::fpe::get_error(struct ubiq_platform_fpe_enc_dec_obj * enc)
{
  std::string ret("Unknown internal error");
  char * err_msg = NULL;
  int err_num;

  if (enc) {
    ubiq_platform_fpe_get_last_error(enc, &err_num, &err_msg);

    if (err_num != 0 && err_msg != NULL) {
      ret = err_msg;
    }
    free(err_msg);
  }
  return ret;
}