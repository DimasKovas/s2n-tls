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

#include "tls/s2n_security_rules.h"

#include <stdarg.h>

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_signature_scheme.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

static S2N_RESULT s2n_security_rule_result_process(struct s2n_security_rule_result *result,
        bool condition, const char *format, ...)
{
    RESULT_ENSURE_REF(result);
    if (condition) {
        return S2N_RESULT_OK;
    }
    result->found_error = true;

    if (!result->write_output) {
        return S2N_RESULT_OK;
    }

    va_list vargs;
    va_start(vargs, format);
    int ret = s2n_stuffer_vprintf(&result->output, format, vargs);
    va_end(vargs);
    RESULT_GUARD_POSIX(ret);
    RESULT_GUARD_POSIX(s2n_stuffer_write_char(&result->output, '\n'));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_security_rule_validate_forward_secret(
        const struct s2n_cipher_suite *cipher_suite, bool *valid)
{
    RESULT_ENSURE_REF(cipher_suite);
    RESULT_ENSURE_REF(cipher_suite->key_exchange_alg);
    *valid = cipher_suite->key_exchange_alg->is_ephemeral;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_security_rule_all_sig_schemes(
        const struct s2n_signature_scheme *sig_scheme, bool *valid)
{
    RESULT_ENSURE_REF(valid);
    *valid = true;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_security_rule_all_curves(
        const struct s2n_ecc_named_curve *curve, bool *valid)
{
    RESULT_ENSURE_REF(valid);
    *valid = true;
    return S2N_RESULT_OK;
}

const struct s2n_security_rule security_rule_definitions[] = {
    [S2N_PERFECT_FORWARD_SECRECY] = {
            .name = "Perfect Forward Secrecy",
            .validate_cipher_suite = s2n_security_rule_validate_forward_secret,
            .validate_sig_scheme = s2n_security_rule_all_sig_schemes,
            .validate_cert_sig_scheme = s2n_security_rule_all_sig_schemes,
            .validate_curve = s2n_security_rule_all_curves,
    },
};

S2N_RESULT s2n_security_rule_validate_policy(const struct s2n_security_rule *rule,
        const struct s2n_security_policy *policy, struct s2n_security_rule_result *result)
{
    RESULT_ENSURE_REF(rule);
    RESULT_ENSURE_REF(policy);
    RESULT_ENSURE_REF(result);

    const char *policy_name = NULL;
    s2n_result_ignore(s2n_security_policy_get_version(policy, &policy_name));
    if (policy_name == NULL) {
        policy_name = "unnamed";
    }

    const char *error_msg_format_name = "%s: policy %s: %s: %s (#%i)";
    const char *error_msg_format_iana = "%s: policy %s: %s: %x (#%i)";

    const struct s2n_cipher_preferences *cipher_prefs = policy->cipher_preferences;
    RESULT_ENSURE_REF(cipher_prefs);
    for (size_t i = 0; i < cipher_prefs->count; i++) {
        const struct s2n_cipher_suite *cipher_suite = cipher_prefs->suites[i];
        RESULT_ENSURE_REF(cipher_suite);
        bool is_valid = false;
        RESULT_GUARD(rule->validate_cipher_suite(cipher_suite, &is_valid));
        RESULT_GUARD(s2n_security_rule_result_process(result, is_valid,
                error_msg_format_name, rule->name, policy_name,
                "cipher suite", cipher_suite->name, i + 1));
    }

    const struct s2n_signature_preferences *sig_prefs = policy->signature_preferences;
    RESULT_ENSURE_REF(sig_prefs);
    for (size_t i = 0; i < sig_prefs->count; i++) {
        const struct s2n_signature_scheme *sig_scheme = sig_prefs->signature_schemes[i];
        RESULT_ENSURE_REF(sig_scheme);
        bool is_valid = false;
        RESULT_GUARD(rule->validate_sig_scheme(sig_scheme, &is_valid));
        RESULT_GUARD(s2n_security_rule_result_process(result, is_valid,
                error_msg_format_iana, rule->name, policy_name,
                "signature scheme", sig_scheme->iana_value, i + 1));
    }

    const struct s2n_signature_preferences *cert_sig_prefs = policy->certificate_signature_preferences;
    if (cert_sig_prefs) {
        for (size_t i = 0; i < cert_sig_prefs->count; i++) {
            const struct s2n_signature_scheme *sig_scheme = cert_sig_prefs->signature_schemes[i];
            RESULT_ENSURE_REF(sig_scheme);
            bool is_valid = false;
            RESULT_GUARD(rule->validate_cert_sig_scheme(sig_scheme, &is_valid));
            RESULT_GUARD(s2n_security_rule_result_process(result, is_valid,
                    error_msg_format_iana, rule->name, policy_name,
                    "certificate signature scheme", sig_scheme->iana_value, i + 1));
        }
    }

    const struct s2n_ecc_preferences *ecc_prefs = policy->ecc_preferences;
    RESULT_ENSURE_REF(ecc_prefs);
    for (size_t i = 0; i < ecc_prefs->count; i++) {
        const struct s2n_ecc_named_curve *curve = ecc_prefs->ecc_curves[i];
        RESULT_ENSURE_REF(curve);
        bool is_valid = false;
        RESULT_GUARD(rule->validate_curve(curve, &is_valid));
        RESULT_GUARD(s2n_security_rule_result_process(result, is_valid,
                error_msg_format_name, rule->name, policy_name,
                "curve", curve->name, i + 1));
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_security_policy_get_security_rules(
        const struct s2n_security_policy *policy, const struct s2n_security_rule **rules,
        size_t max_rules_count, size_t *rules_count)
{
    RESULT_ENSURE_REF(policy);
    RESULT_ENSURE_REF(rules_count);
    *rules_count = 0;

    size_t flags = policy->rules;
    size_t count = 0;
    for (size_t rule = 0; rule < S2N_SECURITY_RULES_COUNT; rule++) {
        bool is_set = flags % 2;
        flags = flags >> 1;

        if (!is_set) {
            continue;
        }

        RESULT_ENSURE_INCLUSIVE_RANGE(0, rule, s2n_array_len(security_rule_definitions));
        RESULT_ENSURE_LT(count, max_rules_count);
        rules[count] = &security_rule_definitions[rule];
        count++;
    }

    *rules_count = count;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_security_policy_validate_security_rules(
        const struct s2n_security_policy *policy, struct s2n_security_rule_result *result)
{
    const struct s2n_security_rule *rules[S2N_SECURITY_RULES_COUNT] = { 0 };
    size_t rules_count = 0;
    RESULT_GUARD(s2n_security_policy_get_security_rules(policy,
            rules, s2n_array_len(rules), &rules_count));
    for (size_t i = 0; i < rules_count; i++) {
        RESULT_GUARD(s2n_security_rule_validate_policy(rules[i], policy, result));
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_security_rule_result_init_output(struct s2n_security_rule_result *result)
{
    /* For the expected, happy case, the rule isn't violated, so nothing is written
     * to the stuffer, so no memory is allocated.
     */
    RESULT_GUARD_POSIX(s2n_stuffer_growable_alloc(&result->output, 0));
    result->write_output = true;
    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_security_rule_result_free(struct s2n_security_rule_result *result)
{
    if (result) {
        RESULT_GUARD_POSIX(s2n_stuffer_free(&result->output));
    }
    *result = (struct s2n_security_rule_result){ 0 };
    return S2N_RESULT_OK;
}
