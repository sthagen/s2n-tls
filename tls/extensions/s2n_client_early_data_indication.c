/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <s2n.h>

#include "tls/extensions/s2n_early_data_indication.h"

#include "tls/extensions/s2n_client_psk.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_early_data.h"
#include "tls/s2n_protocol_preferences.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_safety.h"

static S2N_RESULT s2n_early_data_config_is_possible(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    struct s2n_psk *first_psk = NULL;
    RESULT_GUARD(s2n_array_get(&conn->psk_params.psk_list, 0, (void**) &first_psk));
    RESULT_ENSURE_REF(first_psk);

    struct s2n_early_data_config *early_data_config = &first_psk->early_data_config;

    /* Must support early data */
    RESULT_ENSURE_GT(early_data_config->max_early_data_size, 0);

    /* Early data must require a protocol than we could negotiate */
    RESULT_ENSURE_GTE(s2n_connection_get_protocol_version(conn), early_data_config->protocol_version);
    RESULT_ENSURE_GTE(s2n_connection_get_protocol_version(conn), S2N_TLS13);

    const struct s2n_cipher_preferences *cipher_preferences = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_cipher_preferences(conn, &cipher_preferences));
    RESULT_ENSURE_REF(cipher_preferences);

    /* Early data must require a supported cipher */
    bool match = false;
    for (uint8_t i = 0; i < cipher_preferences->count; i++) {
        if (cipher_preferences->suites[i] == early_data_config->cipher_suite) {
            match = true;
            break;
        }
    }
    RESULT_ENSURE_EQ(match, true);

    /* If early data specifies an application protocol, it must be supported by protocol preferences */
    if (early_data_config->application_protocol.size > 0) {
        struct s2n_blob *application_protocols = NULL;
        RESULT_GUARD_POSIX(s2n_connection_get_protocol_preferences(conn, &application_protocols));
        RESULT_ENSURE_REF(application_protocols);

        match = false;
        RESULT_GUARD(s2n_protocol_preferences_contain(application_protocols, &early_data_config->application_protocol, &match));
        RESULT_ENSURE_EQ(match, true);
    }

    return S2N_RESULT_OK;
}

static bool s2n_client_early_data_indication_should_send(struct s2n_connection *conn)
{
    return s2n_result_is_ok(s2n_early_data_config_is_possible(conn))
            /**
             *= https://tools.ietf.org/rfc/rfc8446#section-4.2.10
             *# A client MUST NOT include the
             *# "early_data" extension in its followup ClientHello.
             **/
            && !s2n_is_hello_retry_handshake(conn)
            /**
             *= https://tools.ietf.org/rfc/rfc8446#section-4.2.10
             *# When a PSK is used and early data is allowed for that PSK, the client
             *# can send Application Data in its first flight of messages.  If the
             *# client opts to do so, it MUST supply both the "pre_shared_key" and
             *# "early_data" extensions.
             */
            && s2n_client_psk_extension.should_send(conn);
}

static int s2n_client_early_data_indication_is_missing(struct s2n_connection *conn)
{
    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_NOT_REQUESTED));
    return S2N_SUCCESS;
}

/**
 * The client version of this extension is empty, so we don't read/write any data.
 *
 *= https://tools.ietf.org/rfc/rfc8446#section-4.2.10
 *# The "extension_data" field of this extension contains an
 *# "EarlyDataIndication" value.
 *#
 *#     struct {} Empty;
 *#
 *#     struct {
 *#         select (Handshake.msg_type) {
 **
 *= https://tools.ietf.org/rfc/rfc8446#section-4.2.10
 *#             case client_hello:         Empty;
 **
 *= https://tools.ietf.org/rfc/rfc8446#section-4.2.10
 *#         };
 *#     } EarlyDataIndication;
 **/

static int s2n_client_early_data_indication_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_REQUESTED));
    return S2N_SUCCESS;
}

static int s2n_client_early_data_indiction_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    /**
     *= https://tools.ietf.org/rfc/rfc8446#section-4.2.10
     *# A client MUST NOT include the
     *# "early_data" extension in its followup ClientHello.
     */
    POSIX_ENSURE(!s2n_is_hello_retry_handshake(conn), S2N_ERR_UNSUPPORTED_EXTENSION);

    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_REQUESTED));
    return S2N_SUCCESS;
}

const s2n_extension_type s2n_client_early_data_indication_extension = {
    .iana_value = TLS_EXTENSION_EARLY_DATA,
    .is_response = false,
    .send = s2n_client_early_data_indication_send,
    .recv = s2n_client_early_data_indiction_recv,
    .should_send = s2n_client_early_data_indication_should_send,
    .if_missing = s2n_client_early_data_indication_is_missing,
};
