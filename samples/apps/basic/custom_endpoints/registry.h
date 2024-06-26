// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.

// CCF
#include "ccf/app_interface.h"
#include "ccf/common_auth_policies.h"
#include "ccf/ds/hash.h"
#include "ccf/http_query.h"
#include "ccf/json_handler.h"
#include "ccf/version.h"

#include <charconv>
#define FMT_HEADER_ONLY
#include <fmt/format.h>

// Custom Endpoints
#include "ccf/bundle.h"
#include "ccf/endpoint.h"
#include "ccf/endpoints/authentication/js.h"
#include "ccf/js/core/context.h"
#include "ccf/js/core/wrapped_property_enum.h"
#include "ccf/js/extensions/ccf/consensus.h"
#include "ccf/js/extensions/ccf/converters.h"
#include "ccf/js/extensions/ccf/crypto.h"
#include "ccf/js/extensions/ccf/historical.h"
#include "ccf/js/extensions/ccf/host.h"
#include "ccf/js/extensions/ccf/kv.h"
#include "ccf/js/extensions/ccf/request.h"
#include "ccf/js/extensions/ccf/rpc.h"
#include "ccf/js/extensions/console.h"
#include "ccf/js/extensions/math/random.h"
#include "ccf/js/modules.h"
#include "ccf/node/rpc_context_impl.h"
#include "js/interpreter_cache_interface.h"

using namespace nlohmann;

namespace basicapp
{
  struct CustomJSEndpoint : public ccf::endpoints::Endpoint
  {};

  class CustomJSEndpointRegistry : public ccf::UserEndpointRegistry
  {
  private:
    std::shared_ptr<ccf::js::AbstractInterpreterCache> interpreter_cache =
      nullptr;
    std::string modules_map;
    std::string metadata_map;
    std::string interpreter_flush_map;
    std::string modules_quickjs_version_map;
    std::string modules_quickjs_bytecode_map;

  public:
    CustomJSEndpointRegistry(
      ccfapp::AbstractNodeContext& context,
      const std::string& kv_prefix_ = "public:custom_endpoints") :
      ccf::UserEndpointRegistry(context),
      modules_map(fmt::format("{}.modules", kv_prefix_)),
      metadata_map(fmt::format("{}.metadata", kv_prefix_)),
      interpreter_flush_map(fmt::format("{}.interpreter_flush", kv_prefix_)),
      modules_quickjs_version_map(
        fmt::format("{}.modules_quickjs_version", kv_prefix_)),
      modules_quickjs_bytecode_map(
        fmt::format("{}.modules_quickjs_bytecode", kv_prefix_))
    {
      interpreter_cache =
        context.get_subsystem<ccf::js::AbstractInterpreterCache>();
      if (interpreter_cache == nullptr)
      {
        throw std::logic_error(
          "Unexpected: Could not access AbstractInterpreterCache subsytem");
      }

      // Install dependency-less (ie reusable) extensions on interpreters _at
      // creation_, rather than on every run
      ccf::js::extensions::Extensions extensions;
      // override Math.random
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::MathRandomExtension>());
      // add console.[debug|log|...]
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::ConsoleExtension>());
      // add ccf.[strToBuf|bufToStr|...]
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::ConvertersExtension>());
      // add ccf.crypto.*
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::CryptoExtension>());
      // add ccf.consensus.*
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::ConsensusExtension>(this));
      // add ccf.host.*
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::HostExtension>(
          context.get_subsystem<ccf::AbstractHostProcesses>().get()));
      // add ccf.historical.*
      extensions.emplace_back(
        std::make_shared<ccf::js::extensions::HistoricalExtension>(
          &context.get_historical_state()));

      interpreter_cache->set_interpreter_factory(
        [extensions](ccf::js::TxAccess access) {
          auto interpreter = std::make_shared<ccf::js::core::Context>(access);

          for (auto extension : extensions)
          {
            interpreter->add_extension(extension);
          }

          return interpreter;
        });
    }

    void install_custom_endpoints(
      ccf::endpoints::EndpointContext& ctx,
      const ccf::js::BundleWrapper& wrapper)
    {
      auto endpoints =
        ctx.tx.template rw<ccf::endpoints::EndpointsMap>(metadata_map);
      endpoints->clear();
      for (const auto& [url, methods] : wrapper.bundle.metadata.endpoints)
      {
        for (const auto& [method, metadata] : methods)
        {
          std::string method_upper = method;
          nonstd::to_upper(method_upper);
          const auto key = ccf::endpoints::EndpointKey{url, method_upper};
          endpoints->put(key, metadata);
        }
      }

      auto modules = ctx.tx.template rw<ccf::Modules>(modules_map);
      modules->clear();
      for (const auto& [name, module] : wrapper.bundle.modules)
      {
        modules->put(fmt::format("/{}", name), module);
      }

      // Trigger interpreter flush, in case interpreter reuse
      // is enabled for some endpoints
      auto interpreter_flush =
        ctx.tx.template rw<ccf::InterpreterFlush>(interpreter_flush_map);
      interpreter_flush->put(true);

      // Refresh app bytecode
      ccf::js::core::Context jsctx(ccf::js::TxAccess::APP_RW);
      jsctx.runtime().set_runtime_options(
        &ctx.tx, ccf::js::core::RuntimeLimitsPolicy::NO_LOWER_THAN_DEFAULTS);
      JS_SetModuleLoaderFunc(
        jsctx.runtime(), nullptr, ccf::js::js_app_module_loader, &ctx.tx);

      auto quickjs_version =
        ctx.tx.wo<ccf::ModulesQuickJsVersion>(modules_quickjs_version_map);
      auto quickjs_bytecode =
        ctx.tx.wo<ccf::ModulesQuickJsBytecode>(modules_quickjs_bytecode_map);

      quickjs_version->put(ccf::quickjs_version);
      quickjs_bytecode->clear();

      modules->foreach([&](const auto& name, const auto& src) {
        auto module_val = ccf::js::load_app_module(
          jsctx,
          name.c_str(),
          &ctx.tx,
          modules_map,
          modules_quickjs_bytecode_map,
          modules_quickjs_version_map);

        uint8_t* out_buf;
        size_t out_buf_len;
        int flags = JS_WRITE_OBJ_BYTECODE;
        out_buf = JS_WriteObject(jsctx, &out_buf_len, module_val.val, flags);
        if (!out_buf)
        {
          throw std::runtime_error(fmt::format(
            "Unable to serialize bytecode for JS module '{}'", name));
        }

        quickjs_bytecode->put(name, {out_buf, out_buf + out_buf_len});
        js_free(jsctx, out_buf);

        return true;
      });
    }

    ccf::endpoints::EndpointDefinitionPtr find_endpoint(
      kv::Tx& tx, ccf::RpcContext& rpc_ctx) override
    {
      // Look up the endpoint definition
      // First in the user-defined endpoints, and then fall-back to built-ins
      const auto method = rpc_ctx.get_method();
      const auto verb = rpc_ctx.get_request_verb();

      auto endpoints = tx.ro<ccf::endpoints::EndpointsMap>(metadata_map);
      const auto key = ccf::endpoints::EndpointKey{method, verb};

      // Look for a direct match of the given path
      const auto it = endpoints->get(key);
      if (it.has_value())
      {
        auto endpoint_def = std::make_shared<CustomJSEndpoint>();
        endpoint_def->dispatch = key;
        endpoint_def->properties = it.value();
        endpoint_def->full_uri_path =
          fmt::format("/{}{}", method_prefix, endpoint_def->dispatch.uri_path);
        ccf::instantiate_authn_policies(*endpoint_def);
        return endpoint_def;
      }

      // If that doesn't exist, look through _all_ the endpoints to find
      // templated matches. If there is one, that's a match. More is an error,
      // none means delegate to the base class.
      {
        std::vector<ccf::endpoints::EndpointDefinitionPtr> matches;

        endpoints->foreach_key([this, &endpoints, &matches, &key, &rpc_ctx](
                                 const auto& other_key) {
          if (key.verb == other_key.verb)
          {
            const auto opt_spec =
              ccf::endpoints::PathTemplateSpec::parse(other_key.uri_path);
            if (opt_spec.has_value())
            {
              const auto& template_spec = opt_spec.value();
              // This endpoint has templates in its path, and the correct verb
              // - now check if template matches the current request's path
              std::smatch match;
              if (std::regex_match(
                    key.uri_path, match, template_spec.template_regex))
              {
                if (matches.empty())
                {
                  auto ctx_impl = static_cast<ccf::RpcContextImpl*>(&rpc_ctx);
                  if (ctx_impl == nullptr)
                  {
                    throw std::logic_error("Unexpected type of RpcContext");
                  }
                  // Populate the request_path_params while we have the match,
                  // though this will be discarded on error if we later find
                  // multiple matches
                  auto& path_params = ctx_impl->path_params;
                  for (size_t i = 0;
                       i < template_spec.template_component_names.size();
                       ++i)
                  {
                    const auto& template_name =
                      template_spec.template_component_names[i];
                    const auto& template_value = match[i + 1].str();
                    path_params[template_name] = template_value;
                  }
                }

                auto endpoint = std::make_shared<ccf::js::JSDynamicEndpoint>();
                endpoint->dispatch = other_key;
                endpoint->full_uri_path = fmt::format(
                  "/{}{}", method_prefix, endpoint->dispatch.uri_path);
                endpoint->properties = endpoints->get(other_key).value();
                ccf::instantiate_authn_policies(*endpoint);
                matches.push_back(endpoint);
              }
            }
          }
          return true;
        });

        if (matches.size() > 1)
        {
          report_ambiguous_templated_path(key.uri_path, matches);
        }
        else if (matches.size() == 1)
        {
          return matches[0];
        }
      }

      return ccf::endpoints::EndpointRegistry::find_endpoint(tx, rpc_ctx);
    }

    using PreExecutionHook = std::function<void(ccf::js::core::Context&)>;

    void do_execute_request(
      const CustomJSEndpoint* endpoint,
      ccf::endpoints::EndpointContext& endpoint_ctx,
      const std::optional<PreExecutionHook>& pre_exec_hook = std::nullopt)
    {
      // This KV Value should be updated by any governance actions which modify
      // the JS app (including _any_ of its contained modules). We then use the
      // version where it was last modified as a safe approximation of when an
      // interpreter is unsafe to use. If this value is written to, the
      // version_of_previous_write will advance, and all cached interpreters
      // will be flushed.
      const auto interpreter_flush =
        endpoint_ctx.tx.ro<ccf::InterpreterFlush>(interpreter_flush_map);
      const auto flush_marker =
        interpreter_flush->get_version_of_previous_write().value_or(0);

      const auto rw_access =
        endpoint->properties.mode == ccf::endpoints::Mode::ReadWrite ?
        ccf::js::TxAccess::APP_RW :
        ccf::js::TxAccess::APP_RO;
      std::optional<ccf::endpoints::InterpreterReusePolicy> reuse_policy =
        endpoint->properties.interpreter_reuse;
      std::shared_ptr<ccf::js::core::Context> interpreter =
        interpreter_cache->get_interpreter(
          rw_access, reuse_policy, flush_marker);
      if (interpreter == nullptr)
      {
        throw std::logic_error("Cache failed to produce interpreter");
      }
      ccf::js::core::Context& ctx = *interpreter;

      // Prevent any other thread modifying this interpreter, until this
      // function completes. We could create interpreters per-thread, but then
      // we would get no cross-thread caching benefit (and would need to either
      // enforce, or share, caps across per-thread caches). We choose
      // instead to allow interpreters to be maximally reused, even across
      // threads, at the cost of locking (and potentially stalling another
      // thread's request execution) here.
      std::lock_guard<ccf::pal::Mutex> guard(ctx.lock);
      // Update the top of the stack for the current thread, used by the stack
      // guard Note this is only active outside SGX
      JS_UpdateStackTop(ctx.runtime());
      // Make the heap and stack limits safe while we init the runtime
      ctx.runtime().reset_runtime_options();

      JS_SetModuleLoaderFunc(
        ctx.runtime(),
        nullptr,
        ccf::js::js_app_module_loader,
        &endpoint_ctx.tx);

      // Extensions with a dependency on this endpoint context (invocation),
      // which must be removed after execution.
      ccf::js::extensions::Extensions local_extensions;

      // ccf.kv.*
      local_extensions.emplace_back(
        std::make_shared<ccf::js::extensions::KvExtension>(&endpoint_ctx.tx));

      // ccf.rpc.*
      local_extensions.emplace_back(
        std::make_shared<ccf::js::extensions::RpcExtension>(
          endpoint_ctx.rpc_ctx.get()));

      auto request_extension =
        std::make_shared<ccf::js::extensions::RequestExtension>(
          endpoint_ctx.rpc_ctx.get());
      local_extensions.push_back(request_extension);

      for (auto extension : local_extensions)
      {
        ctx.add_extension(extension);
      }

      if (pre_exec_hook.has_value())
      {
        pre_exec_hook.value()(ctx);
      }

      ccf::js::core::JSWrappedValue export_func;
      try
      {
        const auto& props = endpoint->properties;
        auto module_val = ccf::js::load_app_module(
          ctx,
          props.js_module.c_str(),
          &endpoint_ctx.tx,
          modules_map,
          modules_quickjs_bytecode_map,
          modules_quickjs_version_map);
        export_func = ctx.get_exported_function(
          module_val, props.js_function, props.js_module);
      }
      catch (const std::exception& exc)
      {
        endpoint_ctx.rpc_ctx->set_error(
          HTTP_STATUS_INTERNAL_SERVER_ERROR,
          ccf::errors::InternalError,
          exc.what());
        return;
      }

      // Call exported function;
      auto request = request_extension->create_request_obj(
        ctx, endpoint->full_uri_path, endpoint_ctx, this);

      auto val = ctx.call_with_rt_options(
        export_func,
        {request},
        &endpoint_ctx.tx,
        ccf::js::core::RuntimeLimitsPolicy::NONE);

      for (auto extension : local_extensions)
      {
        ctx.remove_extension(extension);
      }

      const auto& rt = ctx.runtime();

      if (val.is_exception())
      {
        bool time_out = ctx.interrupt_data.request_timed_out;
        std::string error_msg = "Exception thrown while executing.";
        if (time_out)
        {
          error_msg = "Operation took too long to complete.";
        }

        auto [reason, trace] = ctx.error_message();

        if (rt.log_exception_details)
        {
          CCF_APP_FAIL("{}: {}", reason, trace.value_or("<no trace>"));
        }

        if (rt.return_exception_details)
        {
          std::vector<nlohmann::json> details = {ccf::ODataJSExceptionDetails{
            ccf::errors::JSException, reason, trace}};
          endpoint_ctx.rpc_ctx->set_error(
            HTTP_STATUS_INTERNAL_SERVER_ERROR,
            ccf::errors::InternalError,
            std::move(error_msg),
            std::move(details));
        }
        else
        {
          endpoint_ctx.rpc_ctx->set_error(
            HTTP_STATUS_INTERNAL_SERVER_ERROR,
            ccf::errors::InternalError,
            std::move(error_msg));
        }

        return;
      }

      // Handle return value: {body, headers, statusCode}
      if (!val.is_obj())
      {
        endpoint_ctx.rpc_ctx->set_error(
          HTTP_STATUS_INTERNAL_SERVER_ERROR,
          ccf::errors::InternalError,
          "Invalid endpoint function return value (not an object).");
        return;
      }

      // Response body (also sets a default response content-type header)
      {
        auto response_body_js = val["body"];
        if (!response_body_js.is_undefined())
        {
          std::vector<uint8_t> response_body;
          size_t buf_size;
          size_t buf_offset;
          auto typed_array_buffer = ctx.get_typed_array_buffer(
            response_body_js, &buf_offset, &buf_size, nullptr);
          uint8_t* array_buffer;
          if (!typed_array_buffer.is_exception())
          {
            size_t buf_size_total;
            array_buffer =
              JS_GetArrayBuffer(ctx, &buf_size_total, typed_array_buffer.val);
            array_buffer += buf_offset;
          }
          else
          {
            array_buffer =
              JS_GetArrayBuffer(ctx, &buf_size, response_body_js.val);
          }
          if (array_buffer)
          {
            endpoint_ctx.rpc_ctx->set_response_header(
              http::headers::CONTENT_TYPE,
              http::headervalues::contenttype::OCTET_STREAM);
            response_body =
              std::vector<uint8_t>(array_buffer, array_buffer + buf_size);
          }
          else
          {
            std::optional<std::string> str;
            if (response_body_js.is_str())
            {
              endpoint_ctx.rpc_ctx->set_response_header(
                http::headers::CONTENT_TYPE,
                http::headervalues::contenttype::TEXT);
              str = ctx.to_str(response_body_js);
            }
            else
            {
              endpoint_ctx.rpc_ctx->set_response_header(
                http::headers::CONTENT_TYPE,
                http::headervalues::contenttype::JSON);
              auto rval = ctx.json_stringify(response_body_js);
              if (rval.is_exception())
              {
                auto [reason, trace] = ctx.error_message();

                if (rt.log_exception_details)
                {
                  CCF_APP_FAIL(
                    "Failed to convert return value to JSON:{} {}",
                    reason,
                    trace.value_or("<no trace>"));
                }

                if (rt.return_exception_details)
                {
                  std::vector<nlohmann::json> details = {
                    ccf::ODataJSExceptionDetails{
                      ccf::errors::JSException, reason, trace}};
                  endpoint_ctx.rpc_ctx->set_error(
                    HTTP_STATUS_INTERNAL_SERVER_ERROR,
                    ccf::errors::InternalError,
                    "Invalid endpoint function return value (error during JSON "
                    "conversion of body)",
                    std::move(details));
                }
                else
                {
                  endpoint_ctx.rpc_ctx->set_error(
                    HTTP_STATUS_INTERNAL_SERVER_ERROR,
                    ccf::errors::InternalError,
                    "Invalid endpoint function return value (error during JSON "
                    "conversion of body).");
                }
                return;
              }
              str = ctx.to_str(rval);
            }

            if (!str)
            {
              auto [reason, trace] = ctx.error_message();

              if (rt.log_exception_details)
              {
                CCF_APP_FAIL(
                  "Failed to convert return value to JSON:{} {}",
                  reason,
                  trace.value_or("<no trace>"));
              }

              if (rt.return_exception_details)
              {
                std::vector<nlohmann::json> details = {
                  ccf::ODataJSExceptionDetails{
                    ccf::errors::JSException, reason, trace}};
                endpoint_ctx.rpc_ctx->set_error(
                  HTTP_STATUS_INTERNAL_SERVER_ERROR,
                  ccf::errors::InternalError,
                  "Invalid endpoint function return value (error during string "
                  "conversion of body).",
                  std::move(details));
              }
              else
              {
                endpoint_ctx.rpc_ctx->set_error(
                  HTTP_STATUS_INTERNAL_SERVER_ERROR,
                  ccf::errors::InternalError,
                  "Invalid endpoint function return value (error during string "
                  "conversion of body).");
              }
              return;
            }

            response_body = std::vector<uint8_t>(str->begin(), str->end());
          }
          endpoint_ctx.rpc_ctx->set_response_body(std::move(response_body));
        }
      }

      // Response headers
      {
        auto response_headers_js = val["headers"];
        if (response_headers_js.is_obj())
        {
          ccf::js::core::JSWrappedPropertyEnum prop_enum(
            ctx, response_headers_js);
          for (size_t i = 0; i < prop_enum.size(); i++)
          {
            auto prop_name = ctx.to_str(prop_enum[i]);
            if (!prop_name)
            {
              endpoint_ctx.rpc_ctx->set_error(
                HTTP_STATUS_INTERNAL_SERVER_ERROR,
                ccf::errors::InternalError,
                "Invalid endpoint function return value (header type).");
              return;
            }
            auto prop_val = response_headers_js[*prop_name];
            auto prop_val_str = ctx.to_str(prop_val);
            if (!prop_val_str)
            {
              endpoint_ctx.rpc_ctx->set_error(
                HTTP_STATUS_INTERNAL_SERVER_ERROR,
                ccf::errors::InternalError,
                "Invalid endpoint function return value (header value type).");
              return;
            }
            endpoint_ctx.rpc_ctx->set_response_header(
              *prop_name, *prop_val_str);
          }
        }
      }

      // Response status code
      int response_status_code = HTTP_STATUS_OK;
      {
        auto status_code_js = val["statusCode"];
        if (!status_code_js.is_undefined() && !JS_IsNull(status_code_js.val))
        {
          if (JS_VALUE_GET_TAG(status_code_js.val) != JS_TAG_INT)
          {
            endpoint_ctx.rpc_ctx->set_error(
              HTTP_STATUS_INTERNAL_SERVER_ERROR,
              ccf::errors::InternalError,
              "Invalid endpoint function return value (status code type).");
            return;
          }
          response_status_code = JS_VALUE_GET_INT(status_code_js.val);
        }
        endpoint_ctx.rpc_ctx->set_response_status(response_status_code);
      }
    }

    void execute_request(
      const CustomJSEndpoint* endpoint,
      ccf::endpoints::EndpointContext& endpoint_ctx)
    {
      do_execute_request(endpoint, endpoint_ctx);
    }

    void execute_endpoint(
      ccf::endpoints::EndpointDefinitionPtr e,
      ccf::endpoints::EndpointContext& endpoint_ctx) override
    {
      // Handle endpoint execution
      auto endpoint = dynamic_cast<const CustomJSEndpoint*>(e.get());
      if (endpoint != nullptr)
      {
        execute_request(endpoint, endpoint_ctx);
        return;
      }

      ccf::endpoints::EndpointRegistry::execute_endpoint(e, endpoint_ctx);
    }

    void execute_request_locally_committed(
      const CustomJSEndpoint* endpoint,
      ccf::endpoints::CommandEndpointContext& endpoint_ctx,
      const ccf::TxID& tx_id)
    {
      ccf::endpoints::default_locally_committed_func(endpoint_ctx, tx_id);
    }

    void execute_endpoint_locally_committed(
      ccf::endpoints::EndpointDefinitionPtr e,
      ccf::endpoints::CommandEndpointContext& endpoint_ctx,
      const ccf::TxID& tx_id) override
    {
      auto endpoint = dynamic_cast<const CustomJSEndpoint*>(e.get());
      if (endpoint != nullptr)
      {
        execute_request_locally_committed(endpoint, endpoint_ctx, tx_id);
        return;
      }

      ccf::endpoints::EndpointRegistry::execute_endpoint_locally_committed(
        e, endpoint_ctx, tx_id);
    }
  };
}
