/**
 * @file sipe-cert-crypto-nss.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2011-2012 SIPE Project <http://sipe.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * Certificate routines implementation based on NSS.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#endif

/*
 * Work around a compiler error in NSS 3.13.x. Let's hope they fix it for
 * 3.14.x. See also: https://bugzilla.mozilla.org/show_bug.cgi?id=702090
 */
#include "nss.h"
#if (NSS_VMAJOR == 3) && (NSS_VMINOR == 13)
#define __GNUC_MINOR __GNUC_MINOR__
#endif

#include "cert.h"
#include "cryptohi.h"
#include "keyhi.h"
#include "pk11pub.h"

#include "sipe-backend.h"
#include "sipe-cert-crypto.h"

struct sipe_cert_crypto {
	SECKEYPrivateKey *private;
	SECKEYPublicKey  *public;
};

/*
 * This data structure is used in two different modes
 *
 *  a) certificate generated by the server from our Certificate Request
 *
 *     key_pair.private - reference to client private key, don't free!
 *     key_pair.public  - reference to client public key,  don't free!
 *     decoded          - certificate as NSS data structure, must be freed
 *     raw              - certificate as DER encoded binary, must be freed
 *     length           - length of DER binary
 *
 *  b) server certificate
 *
 *     key_pair.private - NULL
 *     key_pair.public  - reference to server public key, must be freed!
 *     decoded          - certificate as NSS data structure, must be freed
 *     raw              - NULL
 *     length           - modulus length of server public key
 */
struct certificate_nss {
	struct sipe_cert_crypto key_pair;
	CERTCertificate *decoded;
	guchar *raw;
	gsize length;
};

struct sipe_cert_crypto *sipe_cert_crypto_init(void)
{
	PK11SlotInfo *slot = PK11_GetInternalKeySlot();

	if (slot) {
		PK11RSAGenParams rsaParams;
		struct sipe_cert_crypto *scc = g_new0(struct sipe_cert_crypto, 1);

		/* RSA parameters - should those be configurable? */
#ifdef HAVE_VALGRIND
		/*
		 * valgrind makes key pair generation extremely slow. At least
		 * on my system it takes longer for the default key size than
		 * the SIP server timeout and our next message will fail with
		 *
		 *     Read error: Connection reset by peer (104)
		 *
		 * Let's reduce the key size when we detect valgrind.
		 */
		if (RUNNING_ON_VALGRIND) {
			rsaParams.keySizeInBits = 1024;
			SIPE_DEBUG_INFO("sipe_cert_crypto_init: running on valgrind, reducing RSA key size to %d bits",
					rsaParams.keySizeInBits);
		} else
#endif
			rsaParams.keySizeInBits = 2048;
		rsaParams.pe                    = 65537;

		SIPE_DEBUG_INFO_NOFORMAT("sipe_cert_crypto_init: generate key pair, this might take a while...");
		scc->private = PK11_GenerateKeyPair(slot,
						    CKM_RSA_PKCS_KEY_PAIR_GEN,
						    &rsaParams,
						    &scc->public,
						    PR_FALSE, /* not permanent */
						    PR_TRUE,  /* sensitive */
						    NULL);
		if (scc->private) {
			SIPE_DEBUG_INFO_NOFORMAT("sipe_cert_crypto_init: key pair generated");
			PK11_FreeSlot(slot);
			return(scc);
		}

		SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_init: key generation failed");
		g_free(scc);
		PK11_FreeSlot(slot);
	}

	return(NULL);
}

void sipe_cert_crypto_free(struct sipe_cert_crypto *scc)
{
	if (scc) {
		if (scc->public)
			SECKEY_DestroyPublicKey(scc->public);
		if (scc->private)
			SECKEY_DestroyPrivateKey(scc->private);
		g_free(scc);
	}
}

static gchar *sign_cert_or_certreq(CERTCertificate *cert,
				   CERTCertificateRequest *certreq,
				   SECKEYPrivateKey *private)
{
	PRArenaPool *arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
	gchar *base64 = NULL;

	if (arena) {
		SECItem *encoding = SEC_ASN1EncodeItem(arena,
						       NULL,
						       cert ?
						       (void *) cert :
						       (void *) certreq,
						       cert ?
						       SEC_ASN1_GET(CERT_CertificateTemplate) :
						       SEC_ASN1_GET(CERT_CertificateRequestTemplate));

		if (encoding) {
			SECOidTag signtag = SEC_GetSignatureAlgorithmOidTag(private->keyType,
									    SEC_OID_UNKNOWN);

			if (signtag != SEC_OID_UNKNOWN) {
				SECItem raw;

				if (!SEC_DerSignData(arena,
						     &raw,
						     encoding->data,
						     encoding->len,
						     private,
						     signtag)) {

					SIPE_DEBUG_INFO_NOFORMAT("sign_cert_or_certreq: successfully signed");
					base64 = g_base64_encode(raw.data, raw.len);

				} else {
					SIPE_DEBUG_ERROR_NOFORMAT("sign_cert_or_certreq: signing failed");
				}
			} else {
				SIPE_DEBUG_ERROR_NOFORMAT("sign_cert_or_certreq: can't find signature algorithm");
			}

			/* all memory allocated from "arena"
			   SECITEM_FreeItem(encoding, PR_TRUE); */
		} else {
			SIPE_DEBUG_ERROR_NOFORMAT("sign_cert_or_certreq: can't ASN.1 encode data");
		}

		PORT_FreeArena(arena, PR_TRUE);
	} else {
		SIPE_DEBUG_ERROR_NOFORMAT("sign_cert_or_certreq: can't allocate memory");
	}

	return(base64);
}

static CERTCertificateRequest *generate_request(struct sipe_cert_crypto *scc,
						const gchar *subject)
{
	SECItem *pkd;
	CERTCertificateRequest *certreq = NULL;

	if (!scc || !subject)
		return(NULL);

	pkd = SECKEY_EncodeDERSubjectPublicKeyInfo(scc->public);
	if (pkd) {
		CERTSubjectPublicKeyInfo *spki = SECKEY_DecodeDERSubjectPublicKeyInfo(pkd);

		if (spki) {
			gchar *cn      = g_strdup_printf("CN=%s", subject);
			CERTName *name = CERT_AsciiToName(cn);
			g_free(cn);

			if (name) {
				certreq = CERT_CreateCertificateRequest(name,
									spki,
									NULL);
				if (!certreq) {
					SIPE_DEBUG_ERROR_NOFORMAT("generate_request: certreq creation failed");
				}

				CERT_DestroyName(name);
			} else {
				SIPE_DEBUG_ERROR_NOFORMAT("generate_request: subject name creation failed");
			}

			SECKEY_DestroySubjectPublicKeyInfo(spki);
		} else {
			SIPE_DEBUG_ERROR_NOFORMAT("generate_request: DER decode public key info failed");
		}

		SECITEM_FreeItem(pkd, PR_TRUE);
	} else {
		SIPE_DEBUG_ERROR_NOFORMAT("generate_request: DER encode failed");
	}

	return(certreq);
}

gchar *sipe_cert_crypto_request(struct sipe_cert_crypto *scc,
				const gchar *subject)
{
	gchar *base64                   = NULL;
	CERTCertificateRequest *certreq = generate_request(scc, subject);

	if (certreq) {
		base64 = sign_cert_or_certreq(NULL, certreq, scc->private);
		CERT_DestroyCertificateRequest(certreq);
	}

	return(base64);
}

void sipe_cert_crypto_destroy(gpointer certificate)
{
	struct certificate_nss *cn = certificate;

	if (cn) {
		/* imported server certificate - mode (b) */
		if (!cn->raw && cn->key_pair.public)
			SECKEY_DestroyPublicKey(cn->key_pair.public);
		if (cn->decoded)
			CERT_DestroyCertificate(cn->decoded);
		g_free(cn->raw);
		g_free(cn);
	}
}

/* generates certificate_nss in mode (a) */
gpointer sipe_cert_crypto_decode(struct sipe_cert_crypto *scc,
				 const gchar *base64)
{
	struct certificate_nss *cn = g_new0(struct certificate_nss, 1);

	cn->raw     = g_base64_decode(base64, &cn->length);
	cn->decoded = CERT_DecodeCertFromPackage((char *) cn->raw, cn->length);

	if (!cn->decoded) {
		sipe_cert_crypto_destroy(cn);
		return(NULL);
	}

	cn->key_pair = *scc;

	return(cn);
}

/* generates certificate_nss in mode (b) */
gpointer sipe_cert_crypto_import(const guchar *raw,
				 gsize length)
{
	struct certificate_nss *cn = g_new0(struct certificate_nss, 1);

	/* cn->raw not needed as this is a server certificate */
	cn->decoded = CERT_DecodeCertFromPackage((char *) raw, length);

	if (!cn->decoded) {
		sipe_cert_crypto_destroy(cn);
		return(NULL);
	}

	cn->key_pair.public = CERT_ExtractPublicKey(cn->decoded);

	if (!cn->key_pair.public) {
		sipe_cert_crypto_destroy(cn);
		return(NULL);
	}

	cn->length = SECKEY_PublicKeyStrength(cn->key_pair.public);

	return(cn);
}

gboolean sipe_cert_crypto_valid(gpointer certificate,
				guint offset)
{
	struct certificate_nss *cn = certificate;
	SECCertTimeValidity validity;

	if (!cn)
		return(FALSE);

	validity = CERT_CheckCertValidTimes(cn->decoded,
					    /* PRTime unit is microseconds */
					    PR_Now() + offset * PR_USEC_PER_SEC,
					    PR_FALSE);

	return((validity == secCertTimeValid) ||
	       /*
		* From certt.h: "validity could not be decoded from the
		*                cert, most likely because it was NULL"
		*
		* Let's assume if the server sends us such a certificate
		* that it must be valid then...
		*/
	       (validity == secCertTimeUndetermined));
}

guint sipe_cert_crypto_expires(gpointer certificate)
{
	struct certificate_nss *cn = certificate;
	PRTime now, notAfter;

	if (!cn ||
	    (CERT_GetCertTimes(cn->decoded,
			       &now, /* can't be NULL */
			       &notAfter) != SECSuccess))
		return(0);

	/* Sanity check */
	now = PR_Now();
	if (notAfter < now)
		return(0);

	/* PRTime unit is microseconds */
	return((notAfter - now) / PR_USEC_PER_SEC);
}

gsize sipe_cert_crypto_raw_length(gpointer certificate)
{
	return(((struct certificate_nss *) certificate)->length);
}

const guchar *sipe_cert_crypto_raw(gpointer certificate)
{
	return(((struct certificate_nss *) certificate)->raw);
}

gpointer sipe_cert_crypto_public_key(gpointer certificate)
{
	return(((struct certificate_nss *) certificate)->key_pair.public);
}

gsize sipe_cert_crypto_modulus_length(gpointer certificate)
{
	return(((struct certificate_nss *) certificate)->length);
}

gpointer sipe_cert_crypto_private_key(gpointer certificate)
{
	return(((struct certificate_nss *) certificate)->key_pair.private);
}

/* Create test certificate for internal key pair (ONLY USE FOR TEST CODE!!!) */
gpointer sipe_cert_crypto_test_certificate(struct sipe_cert_crypto *scc)
{
	CERTCertificateRequest *certreq = generate_request(scc, "test@test.com");
	struct certificate_nss *cn = NULL;

	if (certreq) {
		/* self-signed */
		CERTName *issuer = CERT_AsciiToName("CN=test@test.com");

		if (issuer) {
			/* we really don't need this certificate for long... */
			CERTValidity *validity = CERT_CreateValidity(PR_Now(),
								     PR_Now() + 600 * PR_USEC_PER_SEC);

			if (validity) {
				CERTCertificate *certificate = CERT_CreateCertificate(1,
										      issuer,
										      validity,
										      certreq);

				if (certificate) {
					SECOidTag signtag = SEC_GetSignatureAlgorithmOidTag(scc->private->keyType,
											    SEC_OID_UNKNOWN);

					if ((signtag != SEC_OID_UNKNOWN) &&
					    (SECOID_SetAlgorithmID(certificate->arena,
								   &certificate->signature,
								   signtag, 0) == SECSuccess)) {
						gchar *base64 = sign_cert_or_certreq(certificate,
										     NULL,
										     scc->private);

						if (base64) {
							cn = sipe_cert_crypto_decode(scc,
										     base64);
							if (!cn) {
								SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_test_certificate: certificate decode failed");
							}

							g_free(base64);
						} else {
							SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_test_certificate: certificate signing failed");
						}
					} else {
						SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_test_certificate: setting certificate signature algorithm ID failed");
					}

					CERT_DestroyCertificate(certificate);
				} else {
					SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_test_certificate: certificate creation failed");
				}

				CERT_DestroyValidity(validity);
			} else {
				SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_test_certificate: validity creation failed");
			}

			CERT_DestroyName(issuer);
		} else {
			SIPE_DEBUG_ERROR_NOFORMAT("sipe_cert_crypto_test_certificate: issuer name creation failed");
		}

		CERT_DestroyCertificateRequest(certreq);
	}

	return(cn);
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
