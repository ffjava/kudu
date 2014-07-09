// Copyright (c) 2014, Cloudera, inc.

#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cmath>
#include <cstdlib>
#include <signal.h>
#include <string>
#include <tr1/memory>
#include <vector>

#include "client/client.h"
#include "client/row_result.h"
#include "client/write_op.h"
#include "common/schema.h"
#include "gutil/gscoped_ptr.h"
#include "gutil/ref_counted.h"
#include "gutil/strings/split.h"
#include "gutil/strings/strcat.h"
#include "gutil/strings/substitute.h"
#include "integration-tests/mini_cluster.h"
#include "master/mini_master.h"
#include "tablet/maintenance_manager.h"
#include "util/async_util.h"
#include "util/countdown_latch.h"
#include "util/errno.h"
#include "util/stopwatch.h"
#include "util/test_macros.h"
#include "util/test_util.h"
#include "util/status.h"
#include "util/subprocess.h"
#include "util/thread.h"
#include "util/random.h"
#include "util/random_util.h"

// Test size parameters
DEFINE_int32(concurrent_inserts, 3, "Number of inserting clients to launch");
DEFINE_int32(inserts_per_client, 500,
             "Number of rows inserted by each inserter client");
DEFINE_int32(rows_per_batch, 125, "Number of rows per client batch");

// Perf-related FLAGS_perf_stat
DEFINE_bool(perf_record_scan, false, "Call \"perf record --call-graph\" "
            "for the durantion of the scan, disabled by default");
DEFINE_bool(perf_stat_scan, false, "Print \"perf stat\" results during"
            "scan to stdout, disabled by default");
DEFINE_bool(perf_fp_flag, false, "Only applicable with --perf_record_scan,"
            " provides argument \"fp\" to the --call-graph flag");

using boost::assign::list_of;
using std::string;
using std::tr1::shared_ptr;
using std::vector;

namespace kudu {
namespace tablet {

using client::Insert;
using client::KuduClient;
using client::KuduClientOptions;
using client::KuduRowResult;
using client::KuduScanner;
using client::KuduSession;
using client::KuduTable;
using strings::Split;
using strings::Substitute;

class FullStackInsertScanTest : public KuduTest {
 protected:
  FullStackInsertScanTest()
    : kNumInsertClients(FLAGS_concurrent_inserts),
    kNumInsertsPerClient(FLAGS_inserts_per_client),
      kNumRows(kNumInsertClients * kNumInsertsPerClient),
    kFlushEveryN(FLAGS_rows_per_batch),
    random_(SeedRandom()),
    // schema has kNumIntCols contiguous columns of Int32 and Int64, in order.
    schema_(list_of
            (ColumnSchema("key", UINT64))
            (ColumnSchema("string_val", STRING))
            (ColumnSchema("int32_val1", INT32))
            (ColumnSchema("int32_val2", INT32))
            (ColumnSchema("int32_val3", INT32))
            (ColumnSchema("int32_val4", INT32))
            (ColumnSchema("int64_val1", INT64))
            (ColumnSchema("int64_val2", INT64))
            (ColumnSchema("int64_val3", INT64))
            (ColumnSchema("int64_val4", INT64)), 1),
    sessions_(kNumInsertClients),
    tables_(kNumInsertClients) {
  }

  const int kNumInsertClients;
  const int kNumInsertsPerClient;
  const int kNumRows;

  virtual void SetUp() OVERRIDE {
    KuduTest::SetUp();
    ASSERT_GE(kNumInsertClients, 0);
    ASSERT_GE(kNumInsertsPerClient, 0);
    InitCluster();
    shared_ptr<KuduClient> reader;
    ASSERT_OK(KuduClient::Create(client_opts_, &reader));
    ASSERT_OK(reader->CreateTable(kTableName, schema_));
    ASSERT_OK(reader->OpenTable(kTableName, &reader_table_));
  }

  virtual void TearDown() OVERRIDE {
    if (cluster_) {
      cluster_->Shutdown();
    }
    KuduTest::TearDown();
  }

  void DoConcurrentClientInserts();
  void DoTestScans();

 private:
  // Generate random row according to schema_.
  static void RandomRow(Random* rng, PartialRow* row,
                        char* buf, uint64_t key, int id);

  void InitCluster() {
    // Start mini-cluster with 1 tserver, config client options
    cluster_.reset(new MiniCluster(env_.get(), test_dir_, 1));
    ASSERT_OK(cluster_->Start());
    client_opts_.master_server_addr =
      cluster_->mini_master()->bound_rpc_addr().ToString();
  }

  // Adds newly generated client's session and table pointers to arrays at id
  void CreateNewClient(int id) {
    shared_ptr<KuduClient> client;
    CHECK_OK(KuduClient::Create(client_opts_, &client));
    CHECK_OK(client->OpenTable(kTableName, &tables_[id]));
    shared_ptr<KuduSession> session = client->NewSession();
    session->SetTimeoutMillis(kSessionTimeoutMs);
    CHECK_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
    sessions_[id] = session;
  }

  // Insert the rows that are associated with that ID.
  void InsertRows(CountDownLatch* start_latch, int id, uint32_t seed);

  // Run a scan from the reader_client_ with the projection schema schema
  // and LOG_TIMING message msg.
  void ScanProjection(const Schema& schema, const string& msg);

  Schema StringSchema() const;
  Schema Int32Schema() const;
  Schema Int64Schema() const;

  static const char* const kTableName;
  static const int kSessionTimeoutMs = 5000;
  static const int kRandomStrMinLength = 16;
  static const int kRandomStrMaxLength = 31;
  static const int kNumIntCols = 4;
  enum {
    kKeyCol,
    kStrCol,
    kInt32ColBase,
    kInt64ColBase = kInt32ColBase + kNumIntCols
  };
  const int kFlushEveryN;

  Random random_;

  Schema schema_;
  shared_ptr<MiniCluster> cluster_;
  KuduClientOptions client_opts_;
  scoped_refptr<KuduTable> reader_table_;
  // Concurrent client insertion test variables
  vector<shared_ptr<KuduSession> > sessions_;
  vector<scoped_refptr<KuduTable> > tables_;
};

namespace {

gscoped_ptr<Subprocess> MakePerfStat() {
  if (!FLAGS_perf_stat_scan) return gscoped_ptr<Subprocess>();
  // No output flag for perf-stat 2.x, just print to output
  string cmd = Substitute("perf stat --pid=$0", getpid());
  LOG(INFO) << "Calling: \"" << cmd << "\"";
  return gscoped_ptr<Subprocess>(new Subprocess("perf", Split(cmd, " ")));
}

gscoped_ptr<Subprocess> MakePerfRecord() {
  if (!FLAGS_perf_record_scan) return gscoped_ptr<Subprocess>();
  string cmd = Substitute("perf record --pid=$0 --call-graph", getpid());
  if (FLAGS_perf_fp_flag) cmd += " fp";
  LOG(INFO) << "Calling: \"" << cmd << "\"";
  return gscoped_ptr<Subprocess>(new Subprocess("perf", Split(cmd, " ")));
}

void InterruptNotNull(gscoped_ptr<Subprocess> sub) {
  if (!sub) return;
  CHECK_OK(sub->Kill(SIGINT));
  int exit_status = 0;
  CHECK_OK(sub->Wait(&exit_status));
  if (!exit_status) {
    LOG(WARNING) << "Subprocess returned " << exit_status
                 << ": " << ErrnoToString(exit_status);
  }
}

// If key is approximately at an even multiple of 1/10 of the way between
// start and end, then a % completion update is printed to LOG(INFO)
// Assumes that end - start + 1 fits into an int
void ReportTenthDone(uint64_t key, uint64_t start, uint64_t end,
                     int id, int numids) {
  int done = key - start + 1;
  int total = end - start + 1;
  if (total < 10) return;
  if (done % (total / 10) == 0) {
    int percent = done * 100 / total;
    LOG(INFO) << "Insertion thread " << id << " of "
              << numids << " is "<< percent << "% done.";
  }
}

void ReportAllDone(int id, int numids) {
  LOG(INFO) << "Insertion thread " << id << " of  "
            << numids << " is 100% done.";
}

} // anonymous namespace

const char* const FullStackInsertScanTest::kTableName = "full-stack-mrs-test-tbl";

TEST_F(FullStackInsertScanTest, MRSOnlyStressTest) {
  MaintenanceManager::Disable();
  DoConcurrentClientInserts();
  DoTestScans();
}

TEST_F(FullStackInsertScanTest, WithDiskStressTest) {
  MaintenanceManager::Enable();
  DoConcurrentClientInserts();
  DoTestScans();
}

void FullStackInsertScanTest::DoConcurrentClientInserts() {
  vector<scoped_refptr<Thread> > threads(kNumInsertClients);
  CountDownLatch start_latch(kNumInsertClients + 1);
  for (int i = 0; i < kNumInsertClients; ++i) {
    CreateNewClient(i);
    ASSERT_OK(Thread::Create(CURRENT_TEST_NAME(),
                             StrCat(CURRENT_TEST_CASE_NAME(), "-id", i),
                             &FullStackInsertScanTest::InsertRows, this,
                             &start_latch, i, random_.Next(), &threads[i]));
    start_latch.CountDown();
  }
  LOG_TIMING(INFO,
             strings::Substitute("concurrent inserts ($0 rows, $1 threads)",
                                 kNumRows, kNumInsertClients)) {
    start_latch.CountDown();
    BOOST_FOREACH(const scoped_refptr<Thread>& thread, threads) {
      ASSERT_OK(ThreadJoiner(thread.get())
                .warn_every_ms(15000)
                .Join());
    }
  }
}

void FullStackInsertScanTest::DoTestScans() {
  LOG(INFO) << "Doing test scans on table of " << kNumRows << " rows.";

  gscoped_ptr<Subprocess> stat = MakePerfStat();
  gscoped_ptr<Subprocess> record = MakePerfRecord();
  if (stat) stat->Start();
  if (record) record->Start();

  ScanProjection(Schema(vector<ColumnSchema>(), 0), "empty projection, 0 col");
  ScanProjection(schema_.CreateKeyProjection(), "key scan, 1 col");
  ScanProjection(schema_, "full schema scan, 10 col");
  ScanProjection(StringSchema(), "String projection, 1 col");
  ScanProjection(Int32Schema(), "Int32 projection, 4 col");
  ScanProjection(Int64Schema(), "Int64 projection, 4 col");

  InterruptNotNull(record.Pass());
  InterruptNotNull(stat.Pass());
}

void FullStackInsertScanTest::InsertRows(CountDownLatch* start_latch, int id,
                                         uint32_t seed) {
  Random rng(seed + id);

  start_latch->Wait();
  // Retrieve id's session and table
  KuduSession* session = CHECK_NOTNULL(sessions_[id].get());
  KuduTable* table = CHECK_NOTNULL(tables_[id].get());
  // Identify start and end of keyrange id is responsible for
  uint64_t start = kNumInsertsPerClient * id;
  uint64_t end = start + kNumInsertsPerClient;
  // Printed id value is in the range 1..kNumInsertClients inclusive
  ++id;
  // Use synchronizer to keep 1 asynchronous batch flush maximum
  Synchronizer sync;
  // Prime the synchronizer as if it was running a batch (for for-loop code)
  sync.AsStatusCallback().Run(Status::OK());
  // Maintain buffer for random string generation
  char randstr[kRandomStrMaxLength + 1];
  // Insert in the id's key range
  for (uint64_t key = start; key < end; ++key) {
    gscoped_ptr<Insert> insert = table->NewInsert();
    RandomRow(&rng, insert->mutable_row(), randstr, key, id);
    CHECK_OK(session->Apply(&insert));

    // Report updates or flush every so often, using the synchronizer to always
    // start filling up the next batch while previous one is sent out.
    if (key % kFlushEveryN == 0) {
      sync.Wait();
      sync.Reset();
      session->FlushAsync(sync.AsStatusCallback());
    }
    ReportTenthDone(key, start, end, id, kNumInsertClients);
  }
  ReportAllDone(id, kNumInsertClients);
  sync.Wait();
  CHECK_OK(session->Flush());
}

void FullStackInsertScanTest::ScanProjection(const Schema& schema,
                                             const string& msg) {
  KuduScanner scanner(reader_table_.get());
  CHECK_OK(scanner.SetProjection(&schema));
  uint64_t nrows = 0;
  LOG_TIMING(INFO, msg) {
    ASSERT_OK(scanner.Open());
    vector<KuduRowResult> rows;
    while (scanner.HasMoreRows()) {
      ASSERT_OK(scanner.NextBatch(&rows));
      nrows += rows.size();
      rows.clear();
    }
  }
  ASSERT_EQ(nrows, kNumRows);
}

// Fills in the fields for a row as defined by the Schema below
// name: (key,      string_val, int32_val$, int64_val$)
// type: (uint64_t, string,     int32_t x4, int64_t x4)
// The first int32 gets the id and the first int64 gets the thread
// id. The key is assigned to "key," and the other fields are random.
void FullStackInsertScanTest::RandomRow(Random* rng, PartialRow* row, char* buf,
                                        uint64_t key, int id) {
  CHECK_OK(row->SetUInt64(kKeyCol, key));
  int len = kRandomStrMinLength +
    rng->Uniform(kRandomStrMaxLength - kRandomStrMinLength + 1);
  RandomString(buf, len, rng);
  buf[len] = '\0';
  CHECK_OK(row->SetStringCopy(kStrCol, buf));
  CHECK_OK(row->SetInt32(kInt32ColBase, id));
  CHECK_OK(row->SetInt64(kInt64ColBase, Thread::current_thread()->tid()));
  for (int i = 1; i < kNumIntCols; ++i) {
    CHECK_OK(row->SetInt32(kInt32ColBase + i, rng->Next32()));
    CHECK_OK(row->SetInt64(kInt64ColBase + i, rng->Next64()));
  }
}

Schema FullStackInsertScanTest::StringSchema() const {
  return Schema(list_of(schema_.column(kKeyCol)), 0);
}

Schema FullStackInsertScanTest::Int32Schema() const {
  vector<ColumnSchema> cols;
  for (int i = 0; i < kNumIntCols; ++i) {
    cols.push_back(schema_.column(kInt32ColBase + i));
  }
  return Schema(cols, 0);
}

Schema FullStackInsertScanTest::Int64Schema() const {
  vector<ColumnSchema> cols;
  for (int i = 0; i < kNumIntCols; ++i) {
    cols.push_back(schema_.column(kInt64ColBase + i));
  }
  return Schema(cols, 0);
}

} // namespace tablet
} // namespace kudu
