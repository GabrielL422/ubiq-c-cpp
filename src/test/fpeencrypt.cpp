#include <gtest/gtest.h>

#include "ubiq/platform.h"

class cpp_fpe_encrypt : public ::testing::Test
{
public:
    void SetUp(void);
    void TearDown(void);

protected:
    ubiq::platform::credentials _creds;
    ubiq::platform::encryption _enc;
};

void cpp_fpe_encrypt::SetUp(void)
{
    ASSERT_TRUE((bool)_creds);
}

void cpp_fpe_encrypt::TearDown(void)
{
}

TEST_F(cpp_fpe_encrypt, none)
{
    ASSERT_NO_THROW(
        _enc = ubiq::platform::encryption(_creds, 1));
}

TEST_F(cpp_fpe_encrypt, simple)
{
    std::string pt("ABC");
    std::vector<std::uint8_t> v;

    ASSERT_NO_THROW(
        v = ubiq::platform::encrypt(_creds, pt.data(), pt.size()));
}

TEST(c_fpe_encrypt, simple)
{
    static const char * const pt = " 01&23-456-78-90";
    static const char * const ffs_name = "ALPHANUM_SSN";

    struct ubiq_platform_credentials * creds;
    char * ctbuf(nullptr);
    size_t ctlen;
    char * ptbuf(nullptr);
    size_t ptlen;
    int res;

    res = ubiq_platform_credentials_create(&creds);
    ASSERT_EQ(res, 0);

    res = ubiq_platform_fpe_encrypt(creds,
      ffs_name, "1", 1, pt, strlen(pt), &ctbuf, &ctlen);
    EXPECT_EQ(res, 0);

    res = ubiq_platform_fpe_decrypt(creds,
      ffs_name, "1", 1, (char *)ctbuf, strlen(ctbuf), &ptbuf, &ptlen);
    EXPECT_EQ(res, 0);

    EXPECT_EQ(strcmp(pt, ptbuf),0);

    ubiq_platform_credentials_destroy(creds);

    free(ctbuf);
    free(ptbuf);
}
