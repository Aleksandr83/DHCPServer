#include "CertificateBuilder.h"

#include <string>
#include <vector>

#include "CertErrorText.h"

#include "esp_log.h"
#include "esp_random.h"

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

using namespace std;

namespace dhcp {
namespace security {

namespace {

const char* TAG = "CertBuilder";

// Rule 39: the sizes and the choices that used to be literals.

/** @brief Room for one PEM block; a P-256 pair is well under a kilobyte. */
constexpr size_t kPemBufBytes = 4096;
/** @brief First serial byte: the top bit is cleared so DER reads it as positive. */
constexpr unsigned char kSerialTopBitMask = 0x7F;
/** @brief Digest of the signature — SHA-256, the usual companion of P-256. */
constexpr mbedtls_md_type_t kSignatureDigest = MBEDTLS_MD_SHA256;
/** @brief A leaf certificate: basicConstraints says "not a certificate authority". */
constexpr int kIsCertificateAuthority = 0;
/** @brief No path length limit; without the CA flag it is not written at all. */
constexpr int kNoPathLengthLimit = 0;
/** @brief X.509 v3 — the version that can carry SAN and key usage extensions. */
constexpr int kCertificateVersion = MBEDTLS_X509_CRT_VERSION_3;
/** @brief `keyUsage`: the key signs (what a TLS server certificates needs). */
constexpr int kKeyUsage = MBEDTLS_X509_KU_DIGITAL_SIGNATURE;

void setDetail(string* detail, const string& text)
{
    if (detail != nullptr) *detail = text;
}

/** @brief PSA status as text; PSA has no portable strerror of its own. */
string psaErrorText(psa_status_t status)
{
    return "PSA error " + to_string(static_cast<int>(status));
}

bool allZero(const unsigned char* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

} // namespace

/**
 * @brief The mbedTLS objects of one attempt, hidden from the header.
 *
 * `keyId` is the PSA key: the key is generated through PSA (the API mbedTLS 4
 * expects) and wrapped into a PK context, which is what the certificate writer
 * needs. `PSA_KEY_ID_NULL` means "nothing generated yet".
 */
struct CertificateBuilder::Impl {
    mbedtls_pk_context key{};
    mbedtls_x509write_cert cert{};
    mbedtls_svc_key_id_t keyId = PSA_KEY_ID_NULL;
};

CertificateBuilder::CertificateBuilder()
    : impl_(new Impl())
{
    mbedtls_pk_init(&impl_->key);
    mbedtls_x509write_crt_init(&impl_->cert);
}

CertificateBuilder::~CertificateBuilder()
{
    reset();
}

void CertificateBuilder::reset()
{
    mbedtls_x509write_crt_free(&impl_->cert);
    mbedtls_x509write_crt_init(&impl_->cert);
    mbedtls_pk_free(&impl_->key);
    mbedtls_pk_init(&impl_->key);
    if (impl_->keyId != PSA_KEY_ID_NULL) {
        psa_destroy_key(impl_->keyId);
        impl_->keyId = PSA_KEY_ID_NULL;
    }
}

bool CertificateBuilder::build(const CertRequest& request, string& certPem,
                               string& keyPem, string* detail)
{
    certPem.clear();
    keyPem.clear();

    if (request.hostName.empty()) {
        setDetail(detail, "the name of the certificate is empty");
        return false;
    }
    reset();                          // a second call must not leak the first key

    if (!generateKey(keyPem, detail)) {
        reset();
        keyPem.clear();
        return false;
    }
    if (!writeCertificate(request, certPem, detail)) {
        reset();
        keyPem.clear();
        certPem.clear();
        return false;
    }
    ESP_LOGI(TAG, "built a self-signed certificate for %s", request.hostName.c_str());
    return true;
}

bool CertificateBuilder::generateKey(string& keyPem, string* detail)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        setDetail(detail, "cannot initialize the crypto library: " + psaErrorText(status));
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, static_cast<size_t>(kKeyBits));
    // One key signs the certificate and then becomes the private key file, so it
    // has to allow signing and to be readable back for the PEM.
    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    status = psa_generate_key(&attributes, &impl_->keyId);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        impl_->keyId = PSA_KEY_ID_NULL;
        setDetail(detail, "cannot generate the key: " + psaErrorText(status));
        return false;
    }

    const int ret = mbedtls_pk_copy_from_psa(impl_->keyId, &impl_->key);
    if (ret != 0) {
        setDetail(detail, "cannot use the generated key: " + mbedErrorText(ret));
        return false;
    }

    // Stage 164: the scratch buffer of the PEM writer is on the heap, not in the
    // frame. Four kilobytes of stack plus the whole mbedTLS write chain is what
    // the httpd task could not carry: the request overflowed its stack, the
    // device panicked, and nothing was written. The generation runs in a task of
    // its own now, and this buffer is not the reason to make that task huge.
    vector<unsigned char> buffer(kPemBufBytes);
    const int written = mbedtls_pk_write_key_pem(&impl_->key, buffer.data(), buffer.size());
    if (written != 0) {
        setDetail(detail, "cannot render the key: " + mbedErrorText(written));
        return false;
    }
    // The PEM writer returns 0, not a length, and terminates the text itself.
    keyPem.assign(reinterpret_cast<const char*>(buffer.data()));
    if (keyPem.empty()) {
        setDetail(detail, "the key came out empty");
        return false;
    }
    ESP_LOGI(TAG, "generated an ECDSA P-%d key", kKeyBits);
    return true;
}

bool CertificateBuilder::writeCertificate(const CertRequest& request, string& certPem,
                                          string* detail)
{
    // Serial: random, and positive when DER reads the first byte as a sign bit.
    // Zero is not a serial number, so an (astronomically unlikely) all-zero draw
    // is replaced by one bit set rather than written out.
    unsigned char serial[kSerialBytes];
    esp_fill_random(serial, sizeof serial);
    serial[0] = static_cast<unsigned char>(serial[0] & kSerialTopBitMask);
    if (allZero(serial, sizeof serial)) serial[kSerialBytes - 1] = 1;

    char notBefore[kTimeTextLen];
    char notAfter[kTimeTextLen];
    if (!asn1Time(request.notBeforeEpoch, notBefore, sizeof notBefore) ||
        !asn1Time(request.notAfterEpoch, notAfter, sizeof notAfter)) {
        setDetail(detail, "the validity dates are outside the supported calendar");
        return false;
    }

    // SAN: the name the operator will type and the address the device answers on,
    // as a list of raw values (mbedTLS 4 writes them into the extension as-is).
    const string& name = request.hostName;
    mbedtls_x509_san_list nameEntry{};
    mbedtls_x509_san_list ipEntry{};
    nameEntry.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    nameEntry.node.san.unstructured_name.p =
        reinterpret_cast<unsigned char*>(const_cast<char*>(name.data()));
    nameEntry.node.san.unstructured_name.len = name.size();
    ipEntry.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    ipEntry.node.san.unstructured_name.p = const_cast<unsigned char*>(request.ip);
    ipEntry.node.san.unstructured_name.len = kIpv4Bytes;
    nameEntry.next = &ipEntry;

    mbedtls_x509write_cert* cert = &impl_->cert;
    mbedtls_x509write_crt_set_version(cert, kCertificateVersion);
    mbedtls_x509write_crt_set_md_alg(cert, kSignatureDigest);
    // Self-signed: the subject is the issuer and one key signs for both.
    mbedtls_x509write_crt_set_subject_key(cert, &impl_->key);
    mbedtls_x509write_crt_set_issuer_key(cert, &impl_->key);

    const string nameDer = commonName(request.hostName);
    int ret = mbedtls_x509write_crt_set_subject_name(cert, nameDer.c_str());
    if (ret != 0) {
        setDetail(detail, "cannot set the subject name: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(cert, nameDer.c_str());
    if (ret != 0) {
        setDetail(detail, "cannot set the issuer name: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_serial_raw(cert, serial, sizeof serial);
    if (ret != 0) {
        setDetail(detail, "cannot set the serial number: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_validity(cert, notBefore, notAfter);
    if (ret != 0) {
        setDetail(detail, "cannot set the validity: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_basic_constraints(cert, kIsCertificateAuthority,
                                                      kNoPathLengthLimit);
    if (ret != 0) {
        setDetail(detail, "cannot set the basic constraints: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_key_usage(cert, kKeyUsage);
    if (ret != 0) {
        setDetail(detail, "cannot set the key usage: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_subject_key_identifier(cert);
    if (ret != 0) {
        setDetail(detail, "cannot set the key identifier: " + mbedErrorText(ret));
        return false;
    }
    ret = mbedtls_x509write_crt_set_subject_alternative_name(cert, &nameEntry);
    if (ret != 0) {
        setDetail(detail, "cannot set the subject alternative name: " + mbedErrorText(ret));
        return false;
    }

    // On the heap as well, for the reason given in generateKey(); this is the
    // frame the crash of stage 164 was measured in (672 bytes plus this buffer).
    vector<unsigned char> buffer(kPemBufBytes);
    const int written = mbedtls_x509write_crt_pem(cert, buffer.data(), buffer.size());
    if (written != 0) {
        setDetail(detail, "cannot write the certificate: " + mbedErrorText(written));
        return false;
    }
    // Like the key writer: 0 means success and the PEM is terminated in place.
    certPem.assign(reinterpret_cast<const char*>(buffer.data()));
    if (certPem.empty()) {
        setDetail(detail, "the certificate came out empty");
        return false;
    }
    return true;
}

} // namespace security
} // namespace dhcp
