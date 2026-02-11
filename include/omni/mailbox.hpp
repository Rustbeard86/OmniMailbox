#pragma once

/**
 * @file mailbox.hpp
 * @brief OmniMailbox public API - single include for the entire library
 * 
 * This is the main entry point for using OmniMailbox. Include this header
 * to access all public types and functions.
 * 
 * @example
 * @code
 * #include <omni/mailbox.hpp>
 * 
 * auto& broker = omni::MailboxBroker::Instance();
 * auto [error, channel] = broker.RequestChannel("my-channel");
 * @endcode
 */

#include "omni/detail/config.hpp"
#include "omni/mailbox_broker.hpp"
#include "omni/producer_handle.hpp"
#include "omni/consumer_handle.hpp"
