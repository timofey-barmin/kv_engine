/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "testapp.h"

#include "ssl_impl.h"

#include <JSON_checker.h>
#include <platform/backtrace.h>
#include <platform/dirutils.h>

#include <gtest/gtest.h>
#include <snappy-c.h>

#include <getopt.h>
#include <fstream>

#ifdef WIN32
#include <process.h>
#define getpid() _getpid()
#endif

McdEnvironment* mcd_env = nullptr;

#define CFG_FILE_PATTERN "memcached_testapp.json.XXXXXX"
using Testapp::MAX_CONNECTIONS;
using Testapp::BACKLOG;

/* test phases (bitmasks) */
#define phase_plain 0x2
#define phase_ssl 0x4

#define phase_max 4
static int current_phase = 0;

pid_t server_pid = pid_t(-1);
in_port_t port = -1;
in_port_t ssl_port = -1;
SOCKET sock = INVALID_SOCKET;
SOCKET sock_ssl;
static std::atomic<bool> allow_closed_read;
static time_t server_start_time = 0;

// used in embedded mode to shutdown memcached
extern void shutdown_server();

std::set<cb::mcbp::Feature> enabled_hello_features;

int memcached_verbose = 0;
// State variable if we're running the memcached server in a
// thread in the same process or not
static bool embedded_memcached_server;

/* static storage for the different environment variables set by
 * putenv().
 *
 * (These must be static as putenv() essentially 'takes ownership' of
 * the provided array, so it is unsafe to use an automatic variable.
 * However, if we use the result of cb_malloc() (i.e. the heap) then
 * memory leak checkers (e.g. Valgrind) will report the memory as
 * leaked as it's impossible to free it).
 */
static char mcd_parent_monitor_env[80];
static char mcd_port_filename_env[80];

static SOCKET connect_to_server_ssl(in_port_t ssl_port);

void set_allow_closed_read(bool enabled) {
    allow_closed_read = enabled;
}

bool sock_is_ssl() {
    return current_phase == phase_ssl;
}

void set_phase_ssl() {
    current_phase = phase_ssl;
}

time_t get_server_start_time() {
    return server_start_time;
}

std::ostream& operator<<(std::ostream& os, const TransportProtocols& t) {
    os << to_string(t);
    return os;
}

std::string to_string(const TransportProtocols& transport) {
#ifdef JETBRAINS_CLION_IDE
    // CLion don't properly parse the output when the
    // output gets written as the string instead of the
    // number. This makes it harder to debug the tests
    // so let's just disable it while we're waiting
    // for them to supply a fix.
    // See https://youtrack.jetbrains.com/issue/CPP-6039
    return std::to_string(static_cast<int>(transport));
#else
    switch (transport) {
    case TransportProtocols::McbpPlain:
        return "Mcbp";
    case TransportProtocols::McbpIpv6Plain:
        return "McbpIpv6";
    case TransportProtocols::McbpSsl:
        return "McbpSsl";
    case TransportProtocols::McbpIpv6Ssl:
        return "McbpIpv6Ssl";
    }
    throw std::logic_error("Unknown transport");
#endif
}

std::string to_string(ClientJSONSupport json) {
    switch (json) {
    case ClientJSONSupport::Yes:
        return "JsonYes";
    case ClientJSONSupport::No:
        return "JsonNo";
    }
    throw std::logic_error("Unknown JSON support");
}

void TestappTest::CreateTestBucket()
{
    auto& conn = connectionMap.getConnection(false);

    // Reconnect to the server so we know we're on a "fresh" connection
    // to the server (and not one that might have been cached by the
    // idle-timer, but not yet noticed on the client side)
    conn.reconnect();
    conn.authenticate("@admin", "password", "PLAIN");

    mcd_env->getTestBucket().setUpBucket(bucketName, "", conn);

    // Reconnect the object to avoid others to reuse the admin creds
    conn.reconnect();
}

void TestappTest::DeleteTestBucket() {
    current_phase = phase_plain;
    sock = connect_to_server_plain(port);
    ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, sasl_auth("@admin",
                                                          "password"));
    union {
        protocol_binary_request_delete_bucket request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;

    size_t plen = mcbp_raw_command(buffer.bytes,
                                   sizeof(buffer.bytes),
                                   PROTOCOL_BINARY_CMD_DELETE_BUCKET,
                                   bucketName.c_str(),
                                   bucketName.size(),
                                   NULL,
                                   0);

    safe_send(buffer.bytes, plen, false);
    safe_recv_packet(&buffer, sizeof(buffer));

    mcbp_validate_response_header(&buffer.response,
                                  PROTOCOL_BINARY_CMD_DELETE_BUCKET,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

TestBucketImpl& TestappTest::GetTestBucket()
{
    return mcd_env->getTestBucket();
}

// Per-test-case set-up.
// Called before the first test in this test case.
void TestappTest::SetUpTestCase() {
    token = 0xdeadbeef;
    memcached_cfg.reset(generate_config(0));
    start_memcached_server(memcached_cfg.get());

    if (HasFailure()) {
        server_pid = reinterpret_cast<pid_t>(-1);
    } else {
        CreateTestBucket();
    }
}

// Per-test-case tear-down.
// Called after the last test in this test case.
void TestappTest::TearDownTestCase() {
    if (sock != INVALID_SOCKET) {
       closesocket(sock);
    }

    if (server_pid != reinterpret_cast<pid_t>(-1)) {
        DeleteTestBucket();
    }
    stop_memcached_server();
}

std::string get_sasl_mechs(void) {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;

    size_t plen = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                   PROTOCOL_BINARY_CMD_SASL_LIST_MECHS,
                                   NULL, 0, NULL, 0);

    safe_send(buffer.bytes, plen, false);
    safe_recv_packet(&buffer, sizeof(buffer));
    mcbp_validate_response_header(&buffer.response,
                                  PROTOCOL_BINARY_CMD_SASL_LIST_MECHS,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);

    std::string ret;
    ret.assign(buffer.bytes + sizeof(buffer.response.bytes),
               buffer.response.message.header.response.getBodylen());
    return ret;
}

struct my_sasl_ctx {
    const char *username;
    cbsasl_secret_t *secret;
};

static int sasl_get_username(void *context, int id, const char **result,
                             unsigned int *len)
{
    struct my_sasl_ctx *ctx = reinterpret_cast<struct my_sasl_ctx *>(context);
    if (!context || !result || (id != CBSASL_CB_USER && id != CBSASL_CB_AUTHNAME)) {
        return CBSASL_BADPARAM;
    }

    *result = ctx->username;
    if (len) {
        *len = (unsigned int)strlen(*result);
    }

    return CBSASL_OK;
}

static int sasl_get_password(cbsasl_conn_t *conn, void *context, int id,
                             cbsasl_secret_t **psecret)
{
    struct my_sasl_ctx *ctx = reinterpret_cast<struct my_sasl_ctx *>(context);
    if (!conn || ! psecret || id != CBSASL_CB_PASS || ctx == NULL) {
        return CBSASL_BADPARAM;
    }

    *psecret = ctx->secret;
    return CBSASL_OK;
}

uint16_t TestappTest::sasl_auth(const char *username, const char *password) {
    cbsasl_error_t err;
    const char *data;
    unsigned int len;
    const char *chosenmech;

    struct my_sasl_ctx context;
    cbsasl_callback_t sasl_callbacks[4];
    cbsasl_conn_t *client;
    std::string mech(get_sasl_mechs());

    sasl_callbacks[0].id = CBSASL_CB_USER;
    sasl_callbacks[0].proc = (int( *)(void)) &sasl_get_username;
    sasl_callbacks[0].context = &context;
    sasl_callbacks[1].id = CBSASL_CB_AUTHNAME;
    sasl_callbacks[1].proc = (int( *)(void)) &sasl_get_username;
    sasl_callbacks[1].context = &context;
    sasl_callbacks[2].id = CBSASL_CB_PASS;
    sasl_callbacks[2].proc = (int( *)(void)) &sasl_get_password;
    sasl_callbacks[2].context = &context;
    sasl_callbacks[3].id = CBSASL_CB_LIST_END;
    sasl_callbacks[3].proc = NULL;
    sasl_callbacks[3].context = NULL;

    context.username = username;
    context.secret = reinterpret_cast<cbsasl_secret_t*>(cb_calloc(1, 100));
    memcpy(context.secret->data, password, strlen(password));
    context.secret->len = (unsigned long)strlen(password);

    err = cbsasl_client_new(NULL, NULL, NULL, NULL, sasl_callbacks, 0, &client);
    EXPECT_EQ(CBSASL_OK, err);
    err = cbsasl_client_start(client, mech.c_str(), NULL, &data, &len, &chosenmech);
    EXPECT_EQ(CBSASL_OK, err);
    if (::testing::Test::HasFailure()) {
        // Can't continue if we didn't suceed in starting SASL auth.
        return PROTOCOL_BINARY_RESPONSE_EINTERNAL;
    }

    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;

    size_t plen = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                   PROTOCOL_BINARY_CMD_SASL_AUTH,
                                   chosenmech, strlen(chosenmech),
                                   data, len);

    safe_send(buffer.bytes, plen, false);
    safe_recv_packet(&buffer, sizeof(buffer));

    bool stepped = false;

    while (buffer.response.message.header.response.status == PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE) {
        stepped = true;
        int datalen = buffer.response.message.header.response.getBodylen() -
                      buffer.response.message.header.response.getKeylen() -
                      buffer.response.message.header.response.extlen;

        int dataoffset = sizeof(buffer.response.bytes) +
                         buffer.response.message.header.response.getKeylen() +
                         buffer.response.message.header.response.extlen;

        err = cbsasl_client_step(client, buffer.bytes + dataoffset, datalen,
                                 NULL, &data, &len);
        EXPECT_EQ(CBSASL_CONTINUE, err);

        plen = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                PROTOCOL_BINARY_CMD_SASL_STEP,
                                chosenmech, strlen(chosenmech), data, len);

        safe_send(buffer.bytes, plen, false);

        safe_recv_packet(&buffer, sizeof(buffer));
    }

    if (stepped) {
        mcbp_validate_response_header(&buffer.response,
                                      PROTOCOL_BINARY_CMD_SASL_STEP,
                                      buffer.response.message.header.response.status);
    } else {
        mcbp_validate_response_header(&buffer.response,
                                      PROTOCOL_BINARY_CMD_SASL_AUTH,
                                      buffer.response.message.header.response.status);
    }
    cb_free(context.secret);
    cbsasl_dispose(&client);

    return buffer.response.message.header.response.status;
}

bool TestappTest::isJSON(cb::const_char_buffer value) {
    JSON_checker::Validator validator;
    const auto* ptr = reinterpret_cast<const uint8_t*>(value.data());
    return validator.validate(ptr, value.size());
}

// per test setup function.
void TestappTest::SetUp() {
    verify_server_running();
    current_phase = phase_plain;
    sock = connect_to_server_plain(port);
    ASSERT_NE(INVALID_SOCKET, sock);

    // Set ewouldblock_engine test harness to default mode.
    ewouldblock_engine_configure(ENGINE_EWOULDBLOCK, EWBEngineMode::First,
                                 /*unused*/0);

    enabled_hello_features.clear();

    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    name.assign(info->test_case_name());
    name.append("_");
    name.append(info->name());
    std::replace(name.begin(), name.end(), '/', '_');
}

// per test tear-down function.
void TestappTest::TearDown() {
    closesocket(sock);
}

const std::string TestappTest::bucketName = "default";

// per test setup function.
void McdTestappTest::SetUp() {
    verify_server_running();
    if (getProtocolParam() == TransportProtocols::McbpPlain) {
        current_phase = phase_plain;
        sock = connect_to_server_plain(port);
        ASSERT_NE(INVALID_SOCKET, sock);
    } else {
        current_phase = phase_ssl;
        sock_ssl = connect_to_server_ssl(ssl_port);
        ASSERT_NE(INVALID_SOCKET, sock_ssl);
    }

    set_json_feature(hasJSONSupport() == ClientJSONSupport::Yes);

    // Set ewouldblock_engine test harness to default mode.
    ewouldblock_engine_configure(ENGINE_EWOULDBLOCK, EWBEngineMode::First,
                                 /*unused*/0);

    setCompressionMode("off");
}

// per test tear-down function.
void McdTestappTest::TearDown() {
    if (getProtocolParam() == TransportProtocols::McbpPlain) {
        closesocket(sock);
    } else {
        closesocket(sock_ssl);
        sock_ssl = INVALID_SOCKET;
        destroy_ssl_socket();
    }
}

ClientJSONSupport McdTestappTest::hasJSONSupport() const {
    return getJSONParam();
}

void TestappTest::setCompressionMode(const std::string compression_mode) {
    mcd_env->getTestBucket().setCompressionMode(
            getConnection(), bucketName, compression_mode);
}

std::string McdTestappTest::PrintToStringCombinedName(
        const ::testing::TestParamInfo<
                ::testing::tuple<TransportProtocols, ClientJSONSupport>>&
                info) {
    return to_string(::testing::get<0>(info.param)) + "_" +
           to_string(::testing::get<1>(info.param));
}

#ifdef WIN32
static void log_network_error(const char* prefix) {
    LPVOID error_msg;
    DWORD err = WSAGetLastError();

    if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, err, 0,
                      (LPTSTR)&error_msg, 0, NULL) != 0) {
        fprintf(stderr, prefix, error_msg);
        LocalFree(error_msg);
    } else {
        fprintf(stderr, prefix, "unknown error");
    }
}
#endif

std::string CERTIFICATE_PATH(const std::string& file) {
#ifdef WIN32
    return std::string("\\tests\\cert\\") + file;
#else
    return std::string("/tests/cert/") + file;
#endif
}
static std::string get_errmaps_dir() {
    std::string dir(SOURCE_ROOT);
    dir += "/etc/couchbase/kv/error_maps";
    cb::io::sanitizePath(dir);
    return dir;
}

cJSON* TestappTest::generate_config(uint16_t ssl_port)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    cJSON *obj = nullptr;
    cJSON *obj_ssl = nullptr;

    const std::string cwd = cb::io::getcwd();
    const std::string pem_path = cwd + CERTIFICATE_PATH("testapp.pem");
    const std::string cert_path = cwd + CERTIFICATE_PATH("testapp.cert");

    if (memcached_verbose > 0) {
        cJSON_AddNumberToObject(root, "verbosity", memcached_verbose);
    } else {
        obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "module", "blackhole_logger.so");
        cJSON_AddItemToArray(array, obj);
    }

    cJSON_AddItemToObject(root, "extensions", array);

    // Build up the interface array
    array = cJSON_CreateArray();

    // One interface using the memcached binary protocol
    obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "port", 0);
    cJSON_AddTrueToObject(obj, "ipv4");
    cJSON_AddTrueToObject(obj, "ipv6");
    cJSON_AddNumberToObject(obj, "maxconn", MAX_CONNECTIONS);
    cJSON_AddNumberToObject(obj, "backlog", BACKLOG);
    cJSON_AddStringToObject(obj, "host", "*");
    cJSON_AddStringToObject(obj, "protocol", "memcached");
    cJSON_AddTrueToObject(obj, "management");
    cJSON_AddItemToArray(array, obj);

    // One interface using the memcached binary protocol over SSL
    obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "port", ssl_port);
    cJSON_AddNumberToObject(obj, "maxconn", MAX_CONNECTIONS);
    cJSON_AddNumberToObject(obj, "backlog", BACKLOG);
    cJSON_AddTrueToObject(obj, "ipv4");
    cJSON_AddTrueToObject(obj, "ipv6");
    cJSON_AddStringToObject(obj, "host", "*");
    cJSON_AddStringToObject(obj, "protocol", "memcached");
    obj_ssl = cJSON_CreateObject();
    cJSON_AddStringToObject(obj_ssl, "key", pem_path.c_str());
    cJSON_AddStringToObject(obj_ssl, "cert", cert_path.c_str());
    cJSON_AddItemToObject(obj, "ssl", obj_ssl);
    cJSON_AddItemToArray(array, obj);

    cJSON_AddItemToObject(root, "interfaces", array);

    cJSON_AddTrueToObject(root, "datatype_json");
    cJSON_AddTrueToObject(root, "datatype_snappy");
    cJSON_AddStringToObject(root, "audit_file",
                            mcd_env->getAuditFilename().c_str());
    cJSON_AddStringToObject(root, "error_maps_dir", get_errmaps_dir().c_str());
    cJSON_AddTrueToObject(root, "xattr_enabled");
    cJSON_AddStringToObject(root, "rbac_file",
                            mcd_env->getRbacFilename().c_str());

    // Add an opcode_attributes_override element so that we know we can
    // parse it if ns_server starts applying one ;-)
    unique_cJSON_ptr kvattr(cJSON_Parse(
            R"({"version":1,"EWB_CTL": {"slow":50}})"));
    cJSON_AddItemToObject(root, "opcode_attributes_override", kvattr.release());

    cJSON_AddFalseToObject(root, "dedupe_nmvb_maps");

    return root;
}

cJSON* TestappTest::generate_config() {
    return generate_config(ssl_port);
}

int write_config_to_file(const std::string& config, const std::string& fname) {
    FILE *fp = fopen(fname.c_str(), "w");

    if (fp == nullptr) {
        return -1;
    } else {
        fprintf(fp, "%s", config.c_str());
        fclose(fp);
    }

    return 0;
}

/**
 * Load and parse the content of a file into a cJSON array
 *
 * @param file the name of the file
 * @return the decoded cJSON representation
 * @throw a string if something goes wrong
 */
unique_cJSON_ptr loadJsonFile(const std::string &file) {
    std::ifstream myfile(file, std::ios::in | std::ios::binary);
    if (!myfile.is_open()) {
        std::string msg("Failed to open file: ");
        msg.append(file);
        throw msg;
    }

    std::string str((std::istreambuf_iterator<char>(myfile)),
                    std::istreambuf_iterator<char>());

    myfile.close();
    unique_cJSON_ptr ret(cJSON_Parse(str.c_str()));
    if (ret.get() == nullptr) {
        std::string msg("Failed to parse file: ");
        msg.append(file);
        throw msg;
    }

    return ret;
}

void TestappTest::verify_server_running() {
    if (embedded_memcached_server) {
        // we don't monitor this thread...
        return ;
    }

    ASSERT_NE(reinterpret_cast<pid_t>(-1), server_pid);

#ifdef WIN32
    DWORD status;
    ASSERT_TRUE(GetExitCodeProcess(server_pid, &status));
    ASSERT_EQ(STILL_ACTIVE, status);
#else

    int status;
    pid_t ret = waitpid(server_pid, &status, WNOHANG);

    EXPECT_NE(static_cast<pid_t>(-1), ret)
                << "waitpid() failed with: " << strerror(errno);
    EXPECT_NE(server_pid, ret)
                << "waitpid status     : " << status << std::endl
                << "WIFEXITED(status)  : " << WIFEXITED(status) << std::endl
                << "WEXITSTATUS(status): " << WEXITSTATUS(status) << std::endl
                << "WIFSIGNALED(status): " << WIFSIGNALED(status) << std::endl
                << "WTERMSIG(status)   : " << WTERMSIG(status) << std::endl
                << "WCOREDUMP(status)  : " << WCOREDUMP(status) << std::endl;
    ASSERT_EQ(0, ret) << "The server isn't running..";
#endif
}

void TestappTest::parse_portnumber_file(in_port_t& port_out,
                                        in_port_t& ssl_port_out,
                                        int timeout) {
    FILE* fp;
    const time_t deadline = time(NULL) + timeout;
    // Wait up to timeout seconds for the port file to be created.
    do {
        usleep(50);
        fp = fopen(portnumber_file.c_str(), "r");
        if (fp != nullptr) {
            break;
        }

        verify_server_running();
    } while (time(NULL) < deadline);

    ASSERT_NE(nullptr, fp) << "Timed out after " << timeout
                           << "s waiting for memcached port file '"
                           << portnumber_file << "' to be created.";
    fclose(fp);

    port_out = (in_port_t)-1;
    ssl_port_out = (in_port_t)-1;

    unique_cJSON_ptr portnumbers;
    portnumbers = loadJsonFile(portnumber_file);
    ASSERT_NE(nullptr, portnumbers);
    connectionMap.initialize(portnumbers.get());

    cJSON* array = cJSON_GetObjectItem(portnumbers.get(), "ports");
    ASSERT_NE(nullptr, array) << "ports not found in portnumber file";

    auto numEntries = cJSON_GetArraySize(array);
    for (int ii = 0; ii < numEntries; ++ii) {
        auto obj = cJSON_GetArrayItem(array, ii);

        auto protocol = cJSON_GetObjectItem(obj, "protocol");
        if (strcmp(protocol->valuestring, "memcached") != 0) {
            // the new test use the connectionmap
            continue;
        }

        auto family = cJSON_GetObjectItem(obj, "family");
        if (strcmp(family->valuestring, "AF_INET") != 0) {
            // For now we don't test IPv6
            continue;
        }
        auto ssl = cJSON_GetObjectItem(obj, "ssl");
        ASSERT_NE(nullptr, ssl);
        auto port = cJSON_GetObjectItem(obj, "port");
        ASSERT_NE(nullptr, port);

        in_port_t* out_port;
        if (ssl->type == cJSON_True) {
            out_port = &ssl_port_out;
        } else {
            out_port = &port_out;
        }
        *out_port = static_cast<in_port_t>(port->valueint);
    }

    EXPECT_EQ(0, remove(portnumber_file.c_str()));
}

extern "C" int memcached_main(int argc, char** argv);

extern "C" void memcached_server_thread_main(void *arg) {
    char *argv[4];
    int argc = 0;
    argv[argc++] = const_cast<char*>("./memcached");
    argv[argc++] = const_cast<char*>("-C");
    argv[argc++] = reinterpret_cast<char*>(arg);

    memcached_main(argc, argv);


}

void TestappTest::spawn_embedded_server() {
    char *filename= mcd_port_filename_env + strlen("MEMCACHED_PORT_FILENAME=");
    snprintf(mcd_port_filename_env, sizeof(mcd_port_filename_env),
             "MEMCACHED_PORT_FILENAME=memcached_ports.%lu.%lu",
             (long)getpid(), (unsigned long)time(NULL));
    remove(filename);
    portnumber_file.assign(filename);
    putenv(mcd_port_filename_env);

    ASSERT_EQ(0, cb_create_thread(&memcached_server_thread,
                                  memcached_server_thread_main,
                                  const_cast<char*>(config_file.c_str()),
                                  0));
}


void TestappTest::start_external_server() {
    char *filename= mcd_port_filename_env + strlen("MEMCACHED_PORT_FILENAME=");
    snprintf(mcd_parent_monitor_env, sizeof(mcd_parent_monitor_env),
             "MEMCACHED_PARENT_MONITOR=%lu", (unsigned long)getpid());
    putenv(mcd_parent_monitor_env);

    snprintf(mcd_port_filename_env, sizeof(mcd_port_filename_env),
             "MEMCACHED_PORT_FILENAME=memcached_ports.%lu.%lu",
             (long)getpid(), (unsigned long)time(NULL));
    remove(filename);
    portnumber_file.assign(filename);
    static char topkeys_env[] = "MEMCACHED_TOP_KEYS=10";
    putenv(topkeys_env);

#ifdef WIN32
    STARTUPINFO sinfo;
    PROCESS_INFORMATION pinfo;
    memset(&sinfo, 0, sizeof(sinfo));
    memset(&pinfo, 0, sizeof(pinfo));
    sinfo.cb = sizeof(sinfo);

    char commandline[1024];
    sprintf(commandline, "memcached.exe -C %s", config_file.c_str());

    putenv(mcd_port_filename_env);

    if (!CreateProcess("memcached.exe", commandline,
                       NULL, NULL, /*bInheritHandles*/FALSE,
                       0,
                       NULL, NULL, &sinfo, &pinfo)) {
        LPVOID error_msg;
        DWORD err = GetLastError();

        if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                          FORMAT_MESSAGE_FROM_SYSTEM |
                          FORMAT_MESSAGE_IGNORE_INSERTS,
                          NULL, err, 0,
                          (LPTSTR)&error_msg, 0, NULL) != 0) {
            fprintf(stderr, "Failed to start process: %s\n", error_msg);
            LocalFree(error_msg);
        } else {
            fprintf(stderr, "Failed to start process: unknown error\n");
        }
        exit(EXIT_FAILURE);
    }

    server_pid = pinfo.hProcess;
#else
    server_pid = fork();
    ASSERT_NE(reinterpret_cast<pid_t>(-1), server_pid);

    if (server_pid == 0) {
        /* Child */
        const char *argv[20];
        int arg = 0;
        putenv(mcd_port_filename_env);

        if (getenv("RUN_UNDER_VALGRIND") != NULL) {
            argv[arg++] = "valgrind";
            argv[arg++] = "--log-file=valgrind.%p.log";
            argv[arg++] = "--leak-check=full";
    #if defined(__APPLE__)
            /* Needed to ensure debugging symbols are up-to-date. */
            argv[arg++] = "--dsymutil=yes";
    #endif
        }
        argv[arg++] = "./memcached";
        argv[arg++] = "-C";
        argv[arg++] = config_file.c_str();

        argv[arg++] = NULL;
        cb_assert(execvp(argv[0], const_cast<char **>(argv)) != -1);
    }
#endif // !WIN32
}

static struct addrinfo *lookuphost(const char *hostname, in_port_t port)
{
    struct addrinfo *ai = 0;
    struct addrinfo hints;
    char service[NI_MAXSERV];
    int error;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;

    (void)snprintf(service, NI_MAXSERV, "%d", port);
    if ((error = getaddrinfo(hostname, service, &hints, &ai)) != 0) {
#ifdef WIN32
        log_network_error("getaddrinfo(): %s\r\n");
#else
       if (error != EAI_SYSTEM) {
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
       } else {
          perror("getaddrinfo()");
       }
#endif
    }

    return ai;
}

SOCKET create_connect_plain_socket(in_port_t port)
{
    struct addrinfo *ai = lookuphost("127.0.0.1", port);
    SOCKET sock = INVALID_SOCKET;
    if (ai != NULL) {
        for (auto* next = ai; next != nullptr && sock == INVALID_SOCKET;
             next = next->ai_next) {
            sock = socket(
                    next->ai_family, next->ai_socktype, next->ai_protocol);
            if (sock != INVALID_SOCKET) {
                if (connect(sock, next->ai_addr, (socklen_t)next->ai_addrlen) ==
                    SOCKET_ERROR) {
                    closesocket(sock);
                    sock = INVALID_SOCKET;
                }
            }
        }
        freeaddrinfo(ai);
    }

    if (sock == INVALID_SOCKET) {
        ADD_FAILURE() << "Failed to connect socket to port: " << port;
        return sock;
    }

    int nodelay_flag = 1;
#if defined(WIN32)
    char* ptr = reinterpret_cast<char*>(&nodelay_flag);
#else
    void* ptr = reinterpret_cast<void*>(&nodelay_flag);
#endif
    EXPECT_EQ(0, setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, ptr,
                            sizeof(nodelay_flag)));

    return sock;
}

SOCKET connect_to_server_plain(in_port_t port) {
    return create_connect_plain_socket(port);
}

static SOCKET connect_to_server_ssl(in_port_t ssl_port) {
    SOCKET sock = create_connect_ssl_socket(ssl_port);
    if (sock == INVALID_SOCKET) {
        ADD_FAILURE() << "Failed to connect SSL socket to port" << ssl_port;
        return INVALID_SOCKET;
    }

    return sock;
}

/*
    re-connect to server.
    Uses global port and ssl_port values.
    New socket-fd written to global "sock" and "ssl_bio"
*/
void reconnect_to_server() {
    if (current_phase == phase_ssl) {
        closesocket(sock_ssl);
        destroy_ssl_socket();

        sock_ssl = connect_to_server_ssl(ssl_port);
        ASSERT_NE(INVALID_SOCKET, sock_ssl);
    } else {
        closesocket(sock);
        sock = connect_to_server_plain(port);
        ASSERT_NE(INVALID_SOCKET, sock);
    }
}

/* Compress the specified document, storing the compressed result in the
 * {deflated}.
 * Caller is responsible for cb_free()ing deflated when no longer needed.
 */
size_t compress_document(const char* data, size_t datalen, char** deflated) {

    // Calculate maximum compressed length and allocate a buffer of that size.
    size_t deflated_len = snappy_max_compressed_length(datalen);
    *deflated = (char*)cb_malloc(deflated_len);

    snappy_status status = snappy_compress(data, datalen, *deflated,
                                           &deflated_len);

    cb_assert(status == SNAPPY_OK);

    return deflated_len;
}

static void set_feature(cb::mcbp::Feature feature, bool enable) {
    // First update the currently enabled features.
    if (enable) {
        enabled_hello_features.insert(feature);
    } else {
        enabled_hello_features.erase(feature);
    }

    // Now send the new HELLO message to the server.
    union {
        protocol_binary_request_hello request;
        protocol_binary_response_hello response;
        char bytes[1024];
    } buffer;
    const char *useragent = "testapp";
    const size_t agentlen = strlen(useragent);

    // Calculate body location and populate the body.
    char* const body_ptr = buffer.bytes + sizeof(buffer.request.message.header);
    memcpy(body_ptr, useragent, agentlen);

    size_t bodylen = agentlen;
    for (auto feature : enabled_hello_features) {
        const uint16_t wire_feature = htons(uint16_t(feature));
        memcpy(body_ptr + bodylen,
               reinterpret_cast<const char*>(&wire_feature), sizeof(wire_feature));
        bodylen += sizeof(wire_feature);
    }

    // Fill in the header at the start of the buffer.
    memset(buffer.bytes, 0, sizeof(buffer.request.message.header));
    buffer.request.message.header.request.magic = PROTOCOL_BINARY_REQ;
    buffer.request.message.header.request.opcode = PROTOCOL_BINARY_CMD_HELLO;
    buffer.request.message.header.request.keylen = htons((uint16_t)agentlen);
    buffer.request.message.header.request.bodylen = htonl(bodylen);

    safe_send(buffer.bytes,
              sizeof(buffer.request.message.header) + bodylen, false);

    safe_recv(&buffer.response, sizeof(buffer.response));
    const size_t response_bodylen =
            buffer.response.message.header.response.getBodylen();
    EXPECT_EQ(bodylen - agentlen, response_bodylen);
    for (auto feature : enabled_hello_features) {
        uint16_t wire_feature;
        safe_recv(&wire_feature, sizeof(wire_feature));
        wire_feature = ntohs(wire_feature);
        EXPECT_EQ(uint16_t(feature), wire_feature);
    }
}

void set_datatype_feature(bool enable) {
    set_feature(cb::mcbp::Feature::JSON, enable);
    set_feature(cb::mcbp::Feature::SNAPPY, enable);
}

std::pair<protocol_binary_response_status, std::string>
fetch_value(const std::string& key) {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } send, receive;
    const size_t len = mcbp_raw_command(send.bytes, sizeof(send.bytes),
                                        PROTOCOL_BINARY_CMD_GET,
                                        key.data(), key.size(), NULL, 0);
    safe_send(send.bytes, len, false);
    EXPECT_TRUE(safe_recv_packet(receive.bytes, sizeof(receive.bytes)));

    protocol_binary_response_status status =
            protocol_binary_response_status(receive.response.message.header.response.status);
    if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        const char* ptr = receive.bytes + sizeof(receive.response) + 4;
        const size_t vallen =
                receive.response.message.header.response.getBodylen() - 4;
        return std::make_pair(PROTOCOL_BINARY_RESPONSE_SUCCESS,
                              std::string(ptr, vallen));
    } else {
        return std::make_pair(status, "");
    }
}

void validate_object(const char *key, const std::string& expected_value) {
    union {
        protocol_binary_request_no_extras request;
        char bytes[1024];
    } send;
    size_t len = mcbp_raw_command(send.bytes, sizeof(send.bytes),
                                  PROTOCOL_BINARY_CMD_GET,
                                  key, strlen(key), NULL, 0);
    safe_send(send.bytes, len, false);

    std::vector<char> receive;
    receive.resize(sizeof(protocol_binary_response_get) + expected_value.size());

    safe_recv_packet(receive.data(), receive.size());

    auto* response = reinterpret_cast<protocol_binary_response_no_extras*>(receive.data());
    mcbp_validate_response_header(response, PROTOCOL_BINARY_CMD_GET,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
    char* ptr = receive.data() + sizeof(*response) + 4;
    size_t vallen = response->message.header.response.getBodylen() - 4;
    ASSERT_EQ(expected_value.size(), vallen);
    std::string actual(ptr, vallen);
    EXPECT_EQ(expected_value, actual);
}

void validate_flags(const char *key, uint32_t expected_flags) {
    union {
        protocol_binary_request_no_extras request;
        char bytes[1024];
    } send;
    size_t len = mcbp_raw_command(send.bytes, sizeof(send.bytes),
                                  PROTOCOL_BINARY_CMD_GET,
                                  key, strlen(key), NULL, 0);
    safe_send(send.bytes, len, false);

    std::vector<char> receive(4096);
    safe_recv_packet(receive.data(), receive.size());

    auto* response = reinterpret_cast<protocol_binary_response_no_extras*>(receive.data());
    mcbp_validate_response_header(response, PROTOCOL_BINARY_CMD_GET,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
    const auto* get_response =
            reinterpret_cast<protocol_binary_response_get*>(receive.data());
    const uint32_t actual_flags = ntohl(get_response->message.body.flags);
    EXPECT_EQ(expected_flags, actual_flags);
}

void store_object(const char *key, const char *value, bool validate) {
    store_object_with_flags(key, value, 0);

    if (validate) {
        validate_object(key, value);
    }
}

void store_object_with_flags(const char *key, const char *value,
                             uint32_t flags) {
    std::vector<char> send;
    send.resize(sizeof(protocol_binary_request_set) + strlen(key) +
                strlen(value));

    size_t len = mcbp_storage_command(send.data(), send.size(),
                                      PROTOCOL_BINARY_CMD_SET,
                                      key, strlen(key), value, strlen(value),
                                      flags, 0);

    safe_send(send.data(), len, false);

    union {
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } receive;
    safe_recv_packet(receive.bytes, sizeof(receive.bytes));
    mcbp_validate_response_header(&receive.response, PROTOCOL_BINARY_CMD_SET,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

void delete_object(const char* key, bool ignore_missing) {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } send, receive;
    size_t len = mcbp_raw_command(send.bytes, sizeof(send.bytes),
                                  PROTOCOL_BINARY_CMD_DELETE, key, strlen(key),
                                  NULL, 0);
    safe_send(send.bytes, len, false);
    safe_recv_packet(receive.bytes, sizeof(receive.bytes));
    if (ignore_missing && receive.response.message.header.response.status ==
            PROTOCOL_BINARY_RESPONSE_KEY_ENOENT) {
        /* Ignore. Just using this for cleanup then */
        return;
    }
    mcbp_validate_response_header(&receive.response, PROTOCOL_BINARY_CMD_DELETE,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

void TestappTest::start_memcached_server(cJSON* config) {

    char config_file_pattern [] = CFG_FILE_PATTERN;
    strncpy(config_file_pattern, CFG_FILE_PATTERN, sizeof(config_file_pattern));
    ASSERT_NE(cb_mktemp(config_file_pattern), nullptr);
    config_file = config_file_pattern;

    char* config_string = cJSON_Print(config);

    ASSERT_EQ(0, write_config_to_file(config_string, config_file.c_str()));
    cJSON_Free(config_string);

    // We need to set MEMCACHED_UNIT_TESTS to enable the use of
    // the ewouldblock engine..
    static char envvar[80];
    snprintf(envvar, sizeof(envvar), "MEMCACHED_UNIT_TESTS=true");
    putenv(envvar);

    server_start_time = time(0);

    if (embedded_memcached_server) {
        spawn_embedded_server();
    } else {
        start_external_server();
    }
    parse_portnumber_file(port, ssl_port, 30);
}

void store_object_w_datatype(const char *key, const void *data, size_t datalen,
                             bool deflate)
{
    protocol_binary_request_no_extras request;
    int keylen = (int)strlen(key);
    char extra[8] = { 0 };
    uint8_t datatype = PROTOCOL_BINARY_RAW_BYTES;
    if (deflate) {
        datatype |= PROTOCOL_BINARY_DATATYPE_SNAPPY;
    }

    memset(request.bytes, 0, sizeof(request));
    request.message.header.request.magic = PROTOCOL_BINARY_REQ;
    request.message.header.request.opcode = PROTOCOL_BINARY_CMD_SET;
    request.message.header.request.datatype = datatype;
    request.message.header.request.extlen = 8;
    request.message.header.request.keylen = htons((uint16_t)keylen);
    request.message.header.request.bodylen = htonl((uint32_t)(keylen + datalen + 8));
    request.message.header.request.opaque = 0xdeadbeef;

    safe_send(&request.bytes, sizeof(request.bytes), false);
    safe_send(extra, sizeof(extra), false);
    safe_send(key, strlen(key), false);
    safe_send(data, datalen, false);

    union {
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } receive;

    safe_recv_packet(receive.bytes, sizeof(receive.bytes));
    mcbp_validate_response_header(&receive.response, PROTOCOL_BINARY_CMD_SET,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

void set_json_feature(bool enable) {
    set_feature(cb::mcbp::Feature::JSON, enable);
}

void set_mutation_seqno_feature(bool enable) {
    set_feature(cb::mcbp::Feature::MUTATION_SEQNO, enable);
}

void set_xattr_feature(bool enable) {
    set_feature(cb::mcbp::Feature::XATTR, enable);
}

void TestappTest::reconfigure(unique_cJSON_ptr& memcached_cfg) {
    current_phase = phase_plain;
    sock = connect_to_server_plain(port);
    write_config_to_file(to_string(memcached_cfg, true), config_file);

    sasl_auth("@admin", "password");
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_CONFIG_RELOAD,
                     nullptr, 0, nullptr, 0);

    safe_send(frame.payload.data(), frame.payload.size(), false);
    union {
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;
    safe_recv_packet(&buffer, sizeof(buffer));
    mcbp_validate_response_header(&buffer.response,
                                  PROTOCOL_BINARY_CMD_CONFIG_RELOAD,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

void TestappTest::waitForShutdown(bool killed) {
#ifdef WIN32
    ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(server_pid, 60000));
    DWORD exit_code = NULL;
    GetExitCodeProcess(server_pid, &exit_code);
    EXPECT_EQ(0, exit_code);
#else
    int status;
    pid_t ret;
    while (true) {
        ret = waitpid(server_pid, &status, 0);
        if (ret == reinterpret_cast<pid_t>(-1) && errno == EINTR) {
            // Just loop again
            continue;
        }
        break;
    }
    ASSERT_NE(reinterpret_cast<pid_t>(-1), ret)
        << "waitpid failed: " << strerror(errno);
    bool correctShutdown = killed ? WIFSIGNALED(status) : WIFEXITED(status);
    EXPECT_TRUE(correctShutdown)
        << "waitpid status     : " << status << std::endl
        << "WIFEXITED(status)  : " << WIFEXITED(status) << std::endl
        << "WEXITSTATUS(status): " << WEXITSTATUS(status) << std::endl
        << "WIFSIGNALED(status): " << WIFSIGNALED(status) << std::endl
        << "WTERMSIG(status)   : " << WTERMSIG(status) << " ("
        << strsignal(WTERMSIG(status)) << ")" << std::endl
        << "WCOREDUMP(status)  : " << WCOREDUMP(status) << std::endl;
    EXPECT_EQ(0, WEXITSTATUS(status));
#endif
    server_pid = reinterpret_cast<pid_t>(-1);
}


void TestappTest::stop_memcached_server() {

    connectionMap.invalidate();
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (embedded_memcached_server) {
        shutdown_server();
        cb_join_thread(memcached_server_thread);
    }

    if (server_pid != reinterpret_cast<pid_t>(-1)) {
#ifdef WIN32
        TerminateProcess(server_pid, 0);
        waitForShutdown();
#else
        if (kill(server_pid, SIGTERM) == 0) {
            waitForShutdown();
        }
#endif
    }

    if (!config_file.empty()) {
        EXPECT_NE(-1, remove(config_file.c_str()));
        config_file.clear();
    }
}


/**
 * Set the session control token in memcached (this token is used
 * to validate the shutdown command)
 */
void TestappTest::setControlToken(void) {
    std::vector<char> message(32);
    mcbp_raw_command(message.data(), message.size(),
                     PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN,
                     nullptr, 0, nullptr, 0);
    char* ptr = reinterpret_cast<char*>(&token);
    memcpy(message.data() + 24, ptr, sizeof(token));
    safe_send(message.data(), message.size(), false);
    uint8_t buffer[1024];
    safe_recv_packet(buffer, sizeof(buffer));
    auto* rsp = reinterpret_cast<protocol_binary_response_no_extras*>(buffer);
    mcbp_validate_response_header(rsp, PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

ssize_t socket_send(SOCKET s, const char *buf, size_t len)
{
#ifdef WIN32
    return send(s, buf, (int)len, 0);
#else
    return send(s, buf, len, 0);
#endif
}

static ssize_t phase_send(const void *buf, size_t len) {
    if (current_phase == phase_ssl) {
        return phase_send_ssl(buf, len);
    } else {
        return socket_send(sock, reinterpret_cast<const char*>(buf), len);
    }
}

ssize_t socket_recv(SOCKET s, char *buf, size_t len)
{
#ifdef WIN32
    return recv(s, buf, (int)len, 0);
#else
    return recv(s, buf, len, 0);
#endif
}

ssize_t phase_recv(void *buf, size_t len) {
    if (current_phase == phase_ssl) {
        return phase_recv_ssl(buf, len);
    } else {
        return socket_recv(sock, reinterpret_cast<char*>(buf), len);
    }
}

char ssl_error_string[256];
int ssl_error_string_len = 256;

static const bool dump_socket_traffic =
        getenv("TESTAPP_PACKET_DUMP") != nullptr;

static char* phase_get_errno() {
    char * rv = 0;
    if (current_phase == phase_ssl) {
        /* could do with more work here, but so far this has sufficed */
        snprintf(ssl_error_string, ssl_error_string_len, "SSL error\n");
        rv = ssl_error_string;
    } else {
        rv = strerror(errno);
    }
    return rv;
}

void safe_send(const void* buf, size_t len, bool hickup)
{
    size_t offset = 0;
    const char* ptr = reinterpret_cast<const char*>(buf);
    do {
        size_t num_bytes = len - offset;
        ssize_t nw;
        if (hickup) {
            if (num_bytes > 1024) {
                num_bytes = (rand() % 1023) + 1;
            }
        }

        nw = phase_send(ptr + offset, num_bytes);

        if (nw == -1) {
            if (errno != EINTR) {
                fprintf(stderr, "Failed to write: %s\n", phase_get_errno());
                print_backtrace_to_file(stderr);
                abort();
            }
        } else {
            if (hickup) {
#ifndef WIN32
                usleep(100);
#endif
            }

            if (dump_socket_traffic) {
                if (current_phase == phase_ssl) {
                    std::cerr << "SSL";
                } else {
                    std::cerr << "PLAIN";
                }
                std::cerr << "> ";
                for (ssize_t ii = 0; ii < nw; ++ii) {
                    std::cerr << "0x" << std::hex << std::setfill('0')
                              << std::setw(2)
                              << uint32_t(*(uint8_t*)(ptr + offset + ii))
                              << ", ";
                }
                std::cerr << std::dec << std::endl;
            }
            offset += nw;
        }
    } while (offset < len);
}

void safe_send(const BinprotCommand& cmd, bool hickup) {
    std::vector<uint8_t> buf;
    cmd.encode(buf);
    safe_send(buf.data(), buf.size(), hickup);
}

bool safe_recv(void *buf, size_t len) {
    size_t offset = 0;
    if (len == 0) {
        return true;
    }
    do {

        ssize_t nr = phase_recv(((char*)buf) + offset, len - offset);

        if (nr == -1) {
            EXPECT_EQ(EINTR, errno) << "Failed to read: " << phase_get_errno();
        } else {
            if (nr == 0 && allow_closed_read) {
                return false;
            }
            EXPECT_NE(0u, nr);
            offset += nr;
        }

        // Give up if we encountered an error.
        if (::testing::Test::HasFailure()) {
            return false;
        }
    } while (offset < len);

    return true;
}

/**
 * Internal function which receives a packet. The type parameter should have
 * these three functions:
 * - resize(n) ->: ensure the buffer has a total capacity for at least n bytes.
 *   This function will usually be called once for the header size (i.e. 24)
 *   and then another time for the total packet size (i.e. 24 + bodylen).
 * - data() -> char*: get the entire buffer
 * - size() -> size_t: get the current size of the buffer
 *
 * @param info an object conforming to the above.
 *
 * Once the function has completed, it will return true if no read errors
 * occurred. The actual size of the packet can be determined by parsing the
 * packet header.
 *
 * See StaticBufInfo which is an implementation that uses a fixed buffer.
 * std::vector naturally conforms to the interface.
 */
template <typename T>
bool safe_recv_packetT(T& info) {
    info.resize(sizeof(protocol_binary_response_header));
    auto *header = reinterpret_cast<protocol_binary_response_header*>(info.data());

    if (!safe_recv(header, sizeof(*header))) {
        return false;
    }

    if (dump_socket_traffic) {
        if (current_phase == phase_ssl) {
            std::cerr << "SSL";
        } else {
            std::cerr << "PLAIN";
        }
        std::cerr << "< ";
        for (size_t ii = 0; ii < sizeof(*header); ++ii) {
            std::cerr << "0x" << std::hex << std::setfill('0') << std::setw(2)
                      << uint32_t(header->bytes[ii]) << ", ";
        }
    }

    header->response.status = ntohs(header->response.status);
    auto bodylen = header->response.getBodylen();

    // Set response to NULL, because the underlying buffer may change.
    header = nullptr;

    info.resize(sizeof(*header) + bodylen);
    auto ret = safe_recv(info.data() + sizeof(*header), bodylen);

    if (dump_socket_traffic) {
        uint8_t* ptr = (uint8_t*)(info.data() + sizeof(*header));
        for (size_t ii = 0; ii < bodylen; ++ii) {
            std::cerr << "0x" << std::hex << std::setfill('0') << std::setw(2)
                      << uint32_t(ptr[ii]) << ", ";
        }
        std::cerr << std::endl;
    }
    return ret;
}

/**
 * Wrapper for a existing buffer which exposes an API suitable for use with
 * safe_recv_packetT()
 */
struct StaticBufInfo {
    StaticBufInfo(void *buf_, size_t len_)
        : buf(reinterpret_cast<char*>(buf_)), len(len_) {
    }
    size_t size(size_t) const { return len; }
    char *data() { return buf; }

    void resize(size_t n) {
        if (n > len) {
            throw std::runtime_error("Cannot enlarge buffer!");
        }
    }

    char *buf;
    const size_t len;
};

bool safe_recv_packet(void *buf, size_t size) {
    StaticBufInfo info(buf, size);
    return safe_recv_packetT(info);
}

bool safe_recv_packet(std::vector<uint8_t>& buf) {
    return safe_recv_packetT(buf);
}

bool safe_recv_packet(BinprotResponse& resp) {
    resp.clear();

    std::vector<uint8_t> buf;
    if (!safe_recv_packet(buf)) {
        return false;
    }
    resp.assign(std::move(buf));
    return true;
}

bool safe_do_command(const BinprotCommand& cmd, BinprotResponse& resp, uint16_t status) {
    safe_send(cmd, false);
    if (!safe_recv_packet(resp)) {
        return false;
    }

    protocol_binary_response_no_extras mcresp;
    mcresp.message.header = const_cast<const BinprotResponse&>(resp).getHeader();
    mcbp_validate_response_header(&mcresp, cmd.getOp(), status);
    return !::testing::Test::HasFailure();
}

// Configues the ewouldblock_engine to use the given mode; value
// is a mode-specific parameter.
void TestappTest::ewouldblock_engine_configure(ENGINE_ERROR_CODE err_code,
                                               const EWBEngineMode& mode,
                                               uint32_t value,
                                               const std::string& key) {
    union {
        request_ewouldblock_ctl request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;

    size_t len = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                  PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL,
                                  key.c_str(), key.size(), NULL, 0);
    buffer.request.message.body.mode = htonl(static_cast<uint32_t>(mode));
    buffer.request.message.body.value = htonl(value);
    buffer.request.message.body.inject_error = htonl(err_code);

    safe_send(buffer.bytes, len, false);

    safe_recv_packet(buffer.bytes, sizeof(buffer.bytes));
    mcbp_validate_response_header(&buffer.response,
                                  PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

void TestappTest::ewouldblock_engine_disable() {
    // Value for err_code doesn't matter...
    ewouldblock_engine_configure(ENGINE_EWOULDBLOCK, EWBEngineMode::Next_N, 0);
}

void TestappTest::reconfigure() {
    write_config_to_file(to_string(memcached_cfg, true), config_file);

    sasl_auth("@admin", "password");
    Frame frame;
    mcbp_raw_command(frame, PROTOCOL_BINARY_CMD_CONFIG_RELOAD,
                     nullptr, 0, nullptr, 0);

    safe_send(frame.payload.data(), frame.payload.size(), false);
    union {
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;
    safe_recv_packet(&buffer, sizeof(buffer));
    mcbp_validate_response_header(&buffer.response,
                                  PROTOCOL_BINARY_CMD_CONFIG_RELOAD,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
    reconnect_to_server();
}

void TestappTest::runCreateXattr(
        const std::string& path,
        const std::string& value,
        bool macro,
        protocol_binary_response_status expectedStatus) {
    auto& connection = getConnection();

    BinprotSubdocCommand cmd;
    cmd.setOp(PROTOCOL_BINARY_CMD_SUBDOC_DICT_ADD);
    cmd.setKey(name);
    cmd.setPath(path);
    cmd.setValue(value);
    if (macro) {
        cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_EXPAND_MACROS |
                         SUBDOC_FLAG_MKDIR_P);
    } else {
        cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
    }

    connection.sendCommand(cmd);

    BinprotResponse resp;
    connection.recvResponse(resp);
    EXPECT_EQ(expectedStatus, resp.getStatus());
}

void TestappTest::createXattr(const std::string& path,
                              const std::string& value,
                              bool macro) {
    runCreateXattr(path, value, macro, PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

BinprotSubdocResponse TestappTest::runGetXattr(
        const std::string& path,
        bool deleted,
        protocol_binary_response_status expectedStatus) {
    auto& connection = getConnection();

    BinprotSubdocCommand cmd;
    cmd.setOp(PROTOCOL_BINARY_CMD_SUBDOC_GET);
    cmd.setKey(name);
    cmd.setPath(path);
    if (deleted) {
        cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH);
        cmd.addDocFlags(mcbp::subdoc::doc_flag::AccessDeleted);
    } else {
        cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH);
    }
    connection.sendCommand(cmd);

    BinprotSubdocResponse resp;
    connection.recvResponse(resp);
    auto status = resp.getStatus();
    if (deleted && status == PROTOCOL_BINARY_RESPONSE_SUBDOC_SUCCESS_DELETED) {
        status = PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }

    if (status != expectedStatus) {
        throw ConnectionError("runGetXattr() failed: ", resp);
    }
    return resp;
}

BinprotSubdocResponse TestappTest::getXattr(const std::string& path,
                                            bool deleted) {
    return runGetXattr(path, deleted, PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

int TestappTest::getResponseCount(protocol_binary_response_status statusCode) {
    unique_cJSON_ptr stats(cJSON_Parse(
            cJSON_GetObjectItem(
                    getConnection().stats("responses detailed").get(),
                    "responses")
                    ->valuestring));
    std::stringstream stream;
    stream << std::hex << statusCode;
    return cJSON_GetObjectItem(stats.get(), stream.str().c_str())->valueint;
}

cb::mcbp::Datatype TestappTest::expectedJSONDatatype() const {
    return hasJSONSupport() == ClientJSONSupport::Yes ? cb::mcbp::Datatype::JSON
                                                      : cb::mcbp::Datatype::Raw;
}

MemcachedConnection& TestappTest::getConnection() {
    return prepare(connectionMap.getConnection());
}

MemcachedConnection& TestappTest::getAdminConnection() {
    auto& conn = getConnection();
    conn.authenticate("@admin", "password", "PLAIN");
    return conn;
}

MemcachedConnection& TestappTest::prepare(MemcachedConnection& connection) {
    connection.reconnect();
    connection.setDatatypeCompressed(true);
    connection.setDatatypeJson(hasJSONSupport() == ClientJSONSupport::Yes);
    connection.setMutationSeqnoSupport(true);
    connection.setXerrorSupport(true);
    connection.setXattrSupport(true);
    return connection;
}

unique_cJSON_ptr TestappTest::memcached_cfg;
std::string TestappTest::portnumber_file;
std::string TestappTest::config_file;
ConnectionMap TestappTest::connectionMap;
uint64_t TestappTest::token;
cb_thread_t TestappTest::memcached_server_thread;

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

#ifndef WIN32
    /*
    ** When running the tests from within CLion it starts the test in
    ** another directory than pwd. This cause us to fail to locate the
    ** memcached binary to start. To work around that lets just do a
    ** chdir(dirname(argv[0])).
    */
    auto testdir = cb::io::dirname(argv[0]);
    if (chdir(testdir.c_str()) != 0) {
        std::cerr << "Failed to change directory to " << testdir << std::endl;
        exit(EXIT_FAILURE);
    }
#endif


#ifdef __sun
    {
        // Use coreadm to set up a corefile pattern to ensure that the corefiles
        // created from the unit tests (of testapp or memcached) don't
        // overwrite each other
        std::string coreadm =
            "coreadm -p core.%%f.%%p " + std::to_string(getpid());
        system(coreadm.c_str());
    }
#endif

    std::string engine_name("default");
    std::string engine_config;

    int cmd;
    while ((cmd = getopt(argc, argv, "vc:eE:")) != EOF) {
        switch (cmd) {
        case 'v':
            memcached_verbose++;
            break;
        case 'c':
            engine_config = optarg;
            break;
        case 'e':
            embedded_memcached_server = true;
            break;
        case 'E':
            engine_name.assign(optarg);
            break;
        default:
            std::cerr << "Usage: " << argv[0] << " [-v] [-e]" << std::endl
                      << std::endl
                      << "  -v Verbose - Print verbose memcached output "
                      << "to stderr." << std::endl
                      << "               (use multiple times to increase the"
                      << " verbosity level." << std::endl
                      << "  -c CONFIG - Additional configuration to pass to "
                      << "bucket creation." << std::endl
                      << "  -e Embedded - Run the memcached daemon in the "
                      << "same process (for debugging only..)" << std::endl
                      << "  -E ENGINE engine type to use. <default|ep>"
                      << std::endl;
            return 1;
        }
    }

    /*
     * If not running in embedded mode we need the McdEnvironment to manageSSL
     * initialization and shutdown.
     */
    mcd_env = new McdEnvironment(
            !embedded_memcached_server, engine_name, engine_config);

    ::testing::AddGlobalTestEnvironment(mcd_env);

    cb_initialize_sockets();

#if !defined(WIN32)
    /*
     * When shutting down SSL connections the SSL layer may attempt to
     * write to the underlying socket. If the socket has been closed
     * on the server side then this will raise a SIGPIPE (and
     * terminate the test program). This is Bad.
     * Therefore ignore SIGPIPE signals; we can use errno == EPIPE if
     * we need that information.
     */
    if (sigignore(SIGPIPE) == -1) {
        std::cerr << "Fatal: failed to ignore SIGPIPE; sigaction" << std::endl;
        return 1;
    }
#endif

    return RUN_ALL_TESTS();
}

/* Request stats
 * @return a map of stat key & values in the server response.
 */
stats_response_t request_stats() {
    union {
        protocol_binary_request_no_extras request;
        protocol_binary_response_no_extras response;
        char bytes[1024];
    } buffer;
    stats_response_t result;

    size_t len = mcbp_raw_command(buffer.bytes, sizeof(buffer.bytes),
                                  PROTOCOL_BINARY_CMD_STAT,
                                  NULL, 0, NULL, 0);

    safe_send(buffer.bytes, len, false);
    while (true) {
        safe_recv_packet(buffer.bytes, sizeof(buffer.bytes));
        mcbp_validate_response_header(&buffer.response,
                                      PROTOCOL_BINARY_CMD_STAT,
                                      PROTOCOL_BINARY_RESPONSE_SUCCESS);

        const char* key_ptr(buffer.bytes + sizeof(buffer.response) +
                            buffer.response.message.header.response.extlen);
        const size_t key_len(
                buffer.response.message.header.response.getKeylen());

        // key length zero indicates end of the stats.
        if (key_len == 0) {
            break;
        }

        const char* val_ptr(key_ptr + key_len);
        const size_t val_len(
                buffer.response.message.header.response.getBodylen() - key_len -
                buffer.response.message.header.response.extlen);

        result.insert(std::make_pair(std::string(key_ptr, key_len),
                                     std::string(val_ptr, val_len)));
    }

    return result;
}

// Extracts a single statistic from the set of stats, returning as
// a uint64_t
uint64_t extract_single_stat(const stats_response_t& stats,
                                      const char* name) {
    auto iter = stats.find(name);
    EXPECT_NE(stats.end(), iter);
    uint64_t result = 0;
    result = std::stoul(iter->second);
    return result;
}

/*
    Using a memcached protocol extesnsion, shift the time
*/
void adjust_memcached_clock(int64_t clock_shift, TimeType timeType) {
    union {
        protocol_binary_adjust_time request;
        protocol_binary_adjust_time_response response;
        char bytes[1024];
    } buffer;

    auto extlen = sizeof(uint64_t) + sizeof(uint8_t);
    size_t len = mcbp_raw_command(buffer.bytes,
                                  sizeof(buffer.bytes),
                                  PROTOCOL_BINARY_CMD_ADJUST_TIMEOFDAY,
                                  NULL,
                                  0,
                                  NULL,
                                  extlen);

    buffer.request.message.header.request.extlen =
            gsl::narrow_cast<uint8_t>(extlen);
    buffer.request.message.body.offset = htonll(clock_shift);
    buffer.request.message.body.timeType = timeType;

    safe_send(buffer.bytes, len, false);
    safe_recv_packet(buffer.bytes, sizeof(buffer.bytes));
    mcbp_validate_response_header(&buffer.response,
                                  PROTOCOL_BINARY_CMD_ADJUST_TIMEOFDAY,
                                  PROTOCOL_BINARY_RESPONSE_SUCCESS);
}
