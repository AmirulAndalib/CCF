// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#include "ccf/ds/ccf_exception.h"
#include "ccf/ds/json.h"
#include "ccf/ds/logger.h"
#include "ccf/pal/locking.h"
#include "ccf/version.h"
#include "common/enclave_interface_types.h"
#include "enclave.h"
#include "enclave_time.h"
#include "ringbuffer_logger.h"

#include <chrono>
#include <cstdint>
#include <thread>

// the central enclave object
static ccf::pal::Mutex create_lock;
static std::atomic<ccf::Enclave*> e;

std::atomic<uint16_t> num_pending_threads = 0;
std::atomic<uint16_t> num_complete_threads = 0;

constexpr size_t min_gap_between_initiation_attempts_us =
  2'000'000; // 2 seconds
std::chrono::microseconds ccf::Channel::min_gap_between_initiation_attempts(
  min_gap_between_initiation_attempts_us);

extern "C"
{
  CreateNodeStatus enclave_create_node(
    void* enclave_config,
    uint8_t* ccf_config,
    size_t ccf_config_size,
    uint8_t* startup_snapshot_data,
    size_t startup_snapshot_size,
    uint8_t* node_cert,
    size_t node_cert_size,
    size_t* node_cert_len,
    uint8_t* service_cert,
    size_t service_cert_size,
    size_t* service_cert_len,
    uint8_t* enclave_version,
    size_t enclave_version_size,
    size_t* enclave_version_len,
    StartType start_type,
    ccf::LoggerLevel log_level,
    size_t num_worker_threads,
    void* time_location,
    const ccf::ds::WorkBeaconPtr& work_beacon)
  {
    std::lock_guard<ccf::pal::Mutex> guard(create_lock);

    if (e != nullptr)
    {
      return CreateNodeStatus::NodeAlreadyCreated;
    }

    EnclaveConfig ec = *static_cast<EnclaveConfig*>(enclave_config);

    // Setup logger to allow enclave logs to reach the host before node is
    // actually created
    auto circuit = std::make_unique<ringbuffer::Circuit>(
      ringbuffer::BufferDef{
        ec.to_enclave_buffer_start,
        ec.to_enclave_buffer_size,
        ec.to_enclave_buffer_offsets},
      ringbuffer::BufferDef{
        ec.from_enclave_buffer_start,
        ec.from_enclave_buffer_size,
        ec.from_enclave_buffer_offsets});
    auto basic_writer_factory =
      std::make_unique<ringbuffer::WriterFactory>(*circuit);
    auto writer_factory = std::make_unique<oversized::WriterFactory>(
      *basic_writer_factory, ec.writer_config);

    // Note: because logger uses ringbuffer, logger can only be initialised once
    // ringbuffer memory has been verified
    auto new_logger = std::make_unique<ccf::RingbufferLogger>(*writer_factory);
    auto* ringbuffer_logger = new_logger.get();
    ccf::logger::config::loggers().push_back(std::move(new_logger));

    {
      auto ccf_version_string = std::string(ccf::ccf_version);
      if (ccf_version_string.size() > enclave_version_size)
      {
        LOG_FAIL_FMT(
          "Version mismatch: host {}, enclave {}",
          ccf_version_string,
          std::string(enclave_version, enclave_version + enclave_version_size));
        return CreateNodeStatus::VersionMismatch;
      }

      // NOLINTBEGIN(bugprone-not-null-terminated-result)
      ::memcpy(
        enclave_version, ccf_version_string.data(), ccf_version_string.size());
      // NOLINTEND(bugprone-not-null-terminated-result)
      *enclave_version_len = ccf_version_string.size();

      num_pending_threads = (uint16_t)num_worker_threads + 1;

      if (num_pending_threads > threading::ThreadMessaging::max_num_threads)
      {
        LOG_FAIL_FMT("Too many threads: {}", num_pending_threads);
        return CreateNodeStatus::TooManyThreads;
      }

      // Initialise singleton instance of ThreadMessaging, now that number of
      // threads are known
      threading::ThreadMessaging::init(num_pending_threads);

      ccf::enclavetime::host_time_us =
        static_cast<decltype(ccf::enclavetime::host_time_us)>(time_location);
    }

    ccf::StartupConfig cc =
      nlohmann::json::parse(ccf_config, ccf_config + ccf_config_size);

    // 2-tx reconfiguration is currently experimental, disable it in release
    // enclaves
    if (
      cc.start.service_configuration.reconfiguration_type.has_value() &&
      cc.start.service_configuration.reconfiguration_type.value() !=
        ccf::ReconfigurationType::ONE_TRANSACTION)
    {
      LOG_FAIL_FMT(
        "2TX reconfiguration is experimental, disabled in release mode");
      return CreateNodeStatus::ReconfigurationMethodNotSupported;
    }

    // Warn if run-time logging level is unsupported. SGX enclaves have their
    // minimum logging level (maximum verbosity) restricted at compile-time,
    // while other platforms can permit any level at compile-time and then bind
    // the run-time choice in attestations.
    const auto mv = ccf::logger::MOST_VERBOSE;
    const auto requested = log_level;
    const auto permitted = std::max(mv, requested);
    if (requested != permitted)
    {
      LOG_FAIL_FMT(
        "Unable to set requested enclave logging level '{}'. Most verbose "
        "permitted level is '{}', so setting level to '{}'.",
        ccf::logger::to_string(requested),
        ccf::logger::to_string(mv),
        ccf::logger::to_string(permitted));
    }

    ccf::logger::config::level() = permitted;

    ccf::Enclave* enclave = nullptr;

    try
    {
      // NOLINTBEGIN(cppcoreguidelines-owning-memory)
      enclave = new ccf::Enclave(
        std::move(circuit),
        std::move(basic_writer_factory),
        std::move(writer_factory),
        ringbuffer_logger,
        cc.ledger_signatures.tx_count,
        cc.ledger_signatures.delay.count_ms(),
        cc.ledger.chunk_size,
        cc.consensus,
        cc.node_certificate.curve_id,
        work_beacon);
      // NOLINTEND(cppcoreguidelines-owning-memory)
    }
    catch (const ccf::ccf_openssl_rdrand_init_error& e)
    {
      LOG_FAIL_FMT(
        "ccf_openssl_rdrand_init_error during enclave init: {}", e.what());
      return CreateNodeStatus::OpenSSLRDRANDInitFailed;
    }
    catch (const std::exception& e)
    {
      // In most places, logging exception messages directly is unsafe
      // because they may contain confidential information. In this
      // instance the chance of confidential information is extremely low
      // - this is early during node startup, when it has not communicated
      // with any other nodes to retrieve confidential state, and any
      // secrets it may have generated are about to be discarded as this
      // node terminates. The debugging benefit is substantial, while the
      // risk is low, so in this case we promote the generic exception
      // message to FAIL.
      LOG_FAIL_FMT("exception during enclave init: {}", e.what());
      return CreateNodeStatus::EnclaveInitFailed;
    }

    CreateNodeStatus status = EnclaveInitFailed;

    try
    {
      std::vector<uint8_t> startup_snapshot(
        startup_snapshot_data, startup_snapshot_data + startup_snapshot_size);
      status = enclave->create_new_node(
        start_type,
        std::move(cc),
        std::move(startup_snapshot),
        node_cert,
        node_cert_size,
        node_cert_len,
        service_cert,
        service_cert_size,
        service_cert_len);
    }
    catch (...)
    {
      // NOLINTBEGIN(cppcoreguidelines-owning-memory)
      delete enclave;
      // NOLINTEND(cppcoreguidelines-owning-memory)
      throw;
    }

    if (status != CreateNodeStatus::OK)
    {
      // NOLINTBEGIN(cppcoreguidelines-owning-memory)
      delete enclave;
      // NOLINTEND(cppcoreguidelines-owning-memory)
      return status;
    }

    e.store(enclave);

    // Reset the thread ID generator. This function will exit before any
    // thread calls enclave_run, and without creating any new threads, so it
    // is safe for the first thread that calls enclave_run to re-use this
    // thread_id. That way they are both considered MAIN_THREAD_ID, even if
    // they are actually distinct std::threads.
    ccf::threading::reset_thread_id_generator();

    return CreateNodeStatus::OK;
  }

  bool enclave_run()
  {
    if (e.load() != nullptr)
    {
      uint16_t tid = 0;
      {
        std::lock_guard<ccf::pal::Mutex> guard(create_lock);

        tid = ccf::threading::get_current_thread_id();
        num_pending_threads.fetch_sub(1);

        LOG_INFO_FMT("Starting thread: {}", tid);
      }

      while (num_pending_threads != 0)
      {
      }

      LOG_INFO_FMT("All threads are ready!");

      if (tid == ccf::threading::MAIN_THREAD_ID)
      {
        auto s = e.load()->run_main();
        while (num_complete_threads !=
               threading::ThreadMessaging::instance().thread_count() - 1)
        {
        }
        threading::ThreadMessaging::shutdown();
        return s;
      }
      auto s = e.load()->run_worker();
      num_complete_threads.fetch_add(1);
      return s;
    }
    return false;
  }
}
