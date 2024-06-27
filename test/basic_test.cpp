#include <gtest/gtest.h>
#include "google/cloud/storage/testing/mock_client.h"

#include <array>
#include <functional>
#include <iostream>
#include <sstream>
#include <memory>

#include "../src/gcsplugin.h"

namespace gc = ::google::cloud;
namespace gcs = gc::storage;

using ::testing::Return;
using LOReturnType = gc::StatusOr<gcs::internal::ListObjectsResponse>;

constexpr int kSuccess{ 0 };
constexpr int kFailure{ 1 };

constexpr int kFalse{ 0 };
constexpr int kTrue{ 1 };


TEST(GCSDriverTest, GetDriverName)
{
    ASSERT_STREQ(driver_getDriverName(), "GCS driver");
}

TEST(GCSDriverTest, GetVersion)
{
    ASSERT_STREQ(driver_getVersion(), "0.1.0");
}

TEST(GCSDriverTest, GetScheme)
{
    ASSERT_STREQ(driver_getScheme(), "gs");
}

TEST(GCSDriverTest, IsReadOnly)
{
    ASSERT_EQ(driver_isReadOnly(), kFalse);
}

TEST(GCSDriverTest, Connect)
{
    //check connection state before call to connect
    ASSERT_EQ(driver_isConnected(), kFalse);

    //call connect and check connection
    ASSERT_EQ(driver_connect(), kSuccess);
    ASSERT_EQ(driver_isConnected(), kTrue);

    //call disconnect and check connection
    ASSERT_EQ(driver_disconnect(), kSuccess);
    ASSERT_EQ(driver_isConnected(), kFalse);
}

TEST(GCSDriverTest, Disconnect)
{
    ASSERT_EQ(driver_connect(), kSuccess);
    ASSERT_EQ(driver_disconnect(), kSuccess);
    ASSERT_EQ(driver_isConnected(), kFalse);
}

TEST(GCSDriverTest, GetFileSize)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_getFileSize("gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/Adult.txt"), 5585568);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, GetMultipartFileSize)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_getFileSize("gs://data-test-khiops-driver-gcs/khiops_data/bq_export/Adult/Adult-split-00000000000*.txt"), 5585568);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, GetSystemPreferredBufferSize)
{
	ASSERT_EQ(driver_getSystemPreferredBufferSize(), 4 * 1024 * 1024);
}

constexpr const char* test_dir_name = "gs://data-test-khiops-driver-gcs/khiops_data/bq_export/Adult/";

constexpr const char* test_single_file = "gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/Adult.txt";
constexpr const char* test_range_file_one_header = "gs://data-test-khiops-driver-gcs/khiops_data/split/Adult/Adult-split-0[0-5].txt";
constexpr const char* test_glob_file_header_each = "gs://data-test-khiops-driver-gcs/khiops_data/bq_export/Adult/*.txt";
constexpr const char* test_double_glob_header_each = "gs://data-test-khiops-driver-gcs/khiops_data/split/Adult_subsplit/**/Adult-split-*.txt";

constexpr std::array<const char*, 4> test_files = {
    test_single_file,
    test_range_file_one_header,
    test_glob_file_header_each,
    test_double_glob_header_each
};


class GCSDriverTestFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mock_client = std::make_shared<gcs::testing::MockClient>();
        auto client = gcs::testing::UndecoratedClientFromMock(mock_client);
        test_setClient(std::move(client));
    }

    void TearDown() override
    {
        test_unsetClient();
    }

public:
    static void TearDownTestSuite()
    {
        ASSERT_EQ(driver_disconnect(), kSuccess);
    }

    std::shared_ptr<gcs::testing::MockClient> mock_client;

    template<typename Func>
    void CheckInvalidURIs(Func f, int expect)
    {
        // null pointer
        ASSERT_EQ(f(nullptr), expect);

        // name without "gs://" prefix
        ASSERT_EQ(f("noprefix"), expect);

        // name with correct prefix, but no clear bucket and object names
        ASSERT_EQ(f("gs://not_valid"), expect);

        // name with only bucket name
        ASSERT_EQ(f("gs://only_bucket_name/"), expect);

        // valid URI, but only object name and assuming global bucket name is not set
        ASSERT_EQ(f("gs:///no_bucket"), expect);
    }
};


gcs::ObjectMetadata MakeObjectMetadata(const std::string& bucket_name, const std::string& name, int64_t generation, uint64_t size)
{
    gcs::ObjectMetadata res;
    res.set_bucket(bucket_name);
    res.set_name(name);
    res.set_generation(generation);
    res.set_size(size);
    std::ostringstream id_oss;
    id_oss << bucket_name << '/' << name << '/' << generation;
    res.set_id(id_oss.str());

    return res;
}

gcs::internal::ListObjectsResponse MakeLOR(const std::string& bucket_name, const std::vector<std::string>& names, std::vector<uint64_t> file_sizes)
{
    gcs::internal::ListObjectsResponse res;
    const size_t count{ names.size() };
    for (size_t i = 0; i < count; i++)
    {
        res.items.push_back(MakeObjectMetadata(bucket_name, names[i], 1, file_sizes[i]));
    }
    return res;
}


TEST_F(GCSDriverTestFixture, FileExists)
{
    CheckInvalidURIs(driver_fileExists, kFalse);

    EXPECT_CALL(*mock_client, ListObjects)
        .WillOnce(Return<LOReturnType>(MakeLOR("mock_bucket", { "mock_name" }, { 10 })))  // file exists
        .WillOnce(Return<LOReturnType>(gcs::internal::ListObjectsResponse{}))             // no file found
        .WillOnce(Return<LOReturnType>({}));                                              // return error

    ASSERT_EQ(driver_fileExists("gs://mock_bucket/mock_name"), kTrue);
    ASSERT_EQ(driver_fileExists("gs://mock_bucket/no_match"), kFalse);
    ASSERT_EQ(driver_fileExists("gs://mock_bucket/error"), kFalse);
}

TEST_F(GCSDriverTestFixture, DirExists)
{
    ASSERT_EQ(driver_dirExists(nullptr), kFalse);
    ASSERT_EQ(driver_dirExists("any_name"), kTrue);
}

#define READ_MOCK_LAMBDA(read_sim) [&](gcs::internal::ReadObjectRangeRequest const& request) {                                                  \
                                        EXPECT_EQ(request.bucket_name(), "mock_bucket") << request;                                             \
                                        std::unique_ptr<gcs::testing::MockObjectReadSource> mock_source{new gcs::testing::MockObjectReadSource};\
                                        ::testing::InSequence seq;                                                                              \
                                        EXPECT_CALL(*mock_source, IsOpen()).WillRepeatedly(Return(true));                                       \
                                        EXPECT_CALL(*mock_source, Read).WillOnce((read_sim));                                                   \
                                        EXPECT_CALL(*mock_source, IsOpen()).WillRepeatedly(Return(false));                                      \
                                                                                                                                                \
                                        return gc::make_status_or<std::unique_ptr<gcs::internal::ObjectReadSource>>(std::move(mock_source));}   \


#define READ_MOCK_LAMBDA_FAILURE [](gcs::internal::ReadObjectRangeRequest const& request) {                                                              \
                                        EXPECT_EQ(request.bucket_name(), "mock_bucket") << request;                                                      \
                                        std::unique_ptr<gcs::testing::MockObjectReadSource> mock_source{new gcs::testing::MockObjectReadSource};         \
                                        ::testing::InSequence seq;                                                                                       \
                                        EXPECT_CALL(*mock_source, IsOpen).WillRepeatedly(Return(true));                                                  \
                                        EXPECT_CALL(*mock_source, Read)                                                                                  \
                                            .WillOnce(Return(google::cloud::Status(google::cloud::StatusCode::kInvalidArgument, "Invalid Argument")));   \
                                        EXPECT_CALL(*mock_source, IsOpen).WillRepeatedly(Return(false));                                                 \
                                                                                                                                                         \
                                        return google::cloud::make_status_or<std::unique_ptr<gcs::internal::ObjectReadSource>>(std::move(mock_source));} \


TEST_F(GCSDriverTestFixture, GetFileSize)
{
    CheckInvalidURIs(driver_getFileSize, -1);

    auto prepare_list_objects = [&](LOReturnType&& result) {
        EXPECT_CALL(*mock_client, ListObjects).WillOnce(Return<LOReturnType>(std::move(result)));
        };

    // dir passed as argument, not a file. same behaviour as "no file found"
    prepare_list_objects(MakeLOR("mock_bucket", {}, {}));
    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/dir_name/"), -1);

    // valid URI, but ListObjects returns unusable data
    prepare_list_objects({});
    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/error"), -1);

    // single file
    constexpr uint64_t expected_size{ 10 };
    prepare_list_objects(MakeLOR("mock_bucket", { "mock_object" }, { expected_size }));
    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/mock_object"), expected_size);


    // tests for multifile cases

    // lambda to simulate the answer to a reading request
    auto simulate_read = [](void* buf, size_t n, const char* content, size_t content_size, size_t& offset) {
        const size_t l = std::min(n, content_size - offset);
        std::memcpy(buf, content + offset, l);
        offset += l;
        return gcs::internal::ReadSourceResult{ l, gcs::internal::HttpResponse{200, {}, {}} };
        };

    // lambda to generate the read simulator lambda, parameterised by content, size and offset
    auto generate_simulator = [&](const char* content, size_t size, size_t& offset) {
        return [&, content, size](void* buf, size_t n) { return simulate_read(buf, n, content, size, offset); };
        };


    // multifile, 2 files, single header

    constexpr auto mock_content_1 = "mock_header\nmock_content_1";
    constexpr auto mock_content_2 = "mock_content_2";
    constexpr size_t mock_header_size{ 12 }; // std::strlen("mock_header\n")    includes end of line char
    constexpr size_t mock_content_1_size{ 26 }; //std::strlen(mock_content_1)
    constexpr size_t mock_content_2_size{ 14 }; //std::strlen(mock_content_2)
    constexpr size_t mock_content_total_size{ mock_content_1_size + mock_content_2_size };

    size_t offset_1{ 0 };
    size_t offset_2{ 0 };


    prepare_list_objects(MakeLOR("mock_bucket", { "mock_file_1", "mock_file_2" }, { mock_content_1_size, mock_content_2_size }));

    EXPECT_CALL(*mock_client, ReadObject)
        .WillOnce(READ_MOCK_LAMBDA(generate_simulator(mock_content_1, mock_content_1_size, offset_1))) //simulate_read_file_1))
        .WillOnce(READ_MOCK_LAMBDA(generate_simulator(mock_content_2, mock_content_2_size, offset_2)));

    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/mock_object"), mock_content_total_size);


    // multifile, 2 files, same header

    constexpr const char* mock_content_3 = "mock_header\nmock_content_3_larger";
    constexpr size_t mock_content_3_size{ 33 };
    constexpr size_t expected_size_common_header{ mock_content_1_size + mock_content_3_size - mock_header_size };

    offset_1 = 0;
    size_t offset_3{ 0 };


    prepare_list_objects(MakeLOR("mock_bucket", { "mock_file_1", "mock_file_3" }, { mock_content_1_size, mock_content_3_size }));

    EXPECT_CALL(*mock_client, ReadObject)
        .WillOnce(READ_MOCK_LAMBDA(generate_simulator(mock_content_1, mock_content_1_size, offset_1)))
        .WillOnce(READ_MOCK_LAMBDA(generate_simulator(mock_content_3, mock_content_3_size, offset_3)));

    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/mock_object"), expected_size_common_header);


    // multifile, with a read failure on first file

    offset_1 = 0;
    offset_3 = 0;


    prepare_list_objects(MakeLOR("mock_bucket", { "mock_file_1", "mock_file_3" }, { mock_content_1_size, mock_content_3_size }));

    EXPECT_CALL(*mock_client, ReadObject).WillOnce(READ_MOCK_LAMBDA_FAILURE);
    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/mock_object"), -1);
    

    //multi file, read failure on second file

    prepare_list_objects(MakeLOR("mock_bucket", { "mock_file_1", "mock_file_3" }, { mock_content_1_size, mock_content_3_size }));

    EXPECT_CALL(*mock_client, ReadObject)
        .WillOnce(READ_MOCK_LAMBDA(generate_simulator(mock_content_1, mock_content_1_size, offset_1)))
        .WillOnce(READ_MOCK_LAMBDA_FAILURE);

    ASSERT_EQ(driver_getFileSize("gs://mock_bucket/mock_object"), -1);
}

TEST_F(GCSDriverTestFixture, OpenReadModeAndClose)
{
    for (const char* file : test_files)
    {
        void* stream = driver_fopen(file, 'r');
        ASSERT_NE(stream, nullptr);
        ASSERT_EQ(driver_fclose(stream), kSuccess);
    }

    //fail on dir
    ASSERT_EQ(driver_fopen(test_dir_name, 'r'), nullptr);

    //fail close on null ptr
    ASSERT_EQ(driver_fclose(nullptr), kFailure);
}