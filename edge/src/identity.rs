use anyhow::{Result, ensure};
use rcgen::{
    BasicConstraints, CertificateParams, CertificateSigningRequestParams, DistinguishedName,
    DnType, ExtendedKeyUsagePurpose, IsCa, Issuer, KeyPair, KeyUsagePurpose, SanType,
};
use serde::{Deserialize, Serialize};
use time::{Duration, OffsetDateTime};
use zeroize::Zeroize;

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct EdgeIdentity {
    pub id: String,
    pub ca_certificate: String,
    ca_key: String,
    pub certificate: String,
    server_key: String,
    pub expires_at: i64,
    pub ca_expires_at: i64,
    // Reissue certificates from the initial implementation without changing stored keys.
    #[serde(default)]
    certificate_has_aki: bool,
}

impl Drop for EdgeIdentity {
    fn drop(&mut self) {
        self.ca_key.zeroize();
        self.server_key.zeroize();
    }
}

impl EdgeIdentity {
    pub fn generate(now: i64) -> Result<Self> {
        ensure!(now >= 1704067200, "system clock is invalid");
        let id = crate::security2::random_hex(16)?;
        let ca_key = KeyPair::generate()?;
        let mut parameters = CertificateParams::new(Vec::<String>::new())?;
        parameters.distinguished_name = DistinguishedName::new();
        parameters
            .distinguished_name
            .push(DnType::CommonName, format!("Prone Guard {id}"));
        parameters.not_before = OffsetDateTime::from_unix_timestamp(now)? - Duration::minutes(5);
        parameters.not_after = OffsetDateTime::from_unix_timestamp(now)? + Duration::days(3650);
        parameters.is_ca = IsCa::Ca(BasicConstraints::Constrained(0));
        parameters.key_usages = vec![KeyUsagePurpose::KeyCertSign, KeyUsagePurpose::CrlSign];
        let ca_certificate = parameters.self_signed(&ca_key)?.pem();
        let mut identity = Self {
            id,
            ca_certificate,
            ca_key: ca_key.serialize_pem(),
            certificate: String::new(),
            server_key: KeyPair::generate()?.serialize_pem(),
            expires_at: 0,
            ca_expires_at: parameters.not_after.unix_timestamp(),
            certificate_has_aki: false,
        };
        identity.renew(now)?;
        Ok(identity)
    }

    pub fn hostname(&self) -> String {
        format!("pg-edge-{}.local", self.id)
    }

    pub fn tls_pem(&self) -> (Vec<u8>, Vec<u8>) {
        (
            format!("{}{}", self.certificate, self.ca_certificate).into_bytes(),
            self.server_key.as_bytes().to_vec(),
        )
    }

    pub fn renew(&mut self, now: i64) -> Result<bool> {
        ensure!(
            crate::pairing::valid_identity(&self.id)
                && now >= 1704067200
                && now < self.ca_expires_at,
            "edge identity or CA lifetime is invalid; authenticated USB recovery required"
        );
        if self.expires_at > now + 90 * 86400 && self.certificate_has_aki {
            return Ok(false);
        }
        let issuer =
            Issuer::from_ca_cert_pem(&self.ca_certificate, KeyPair::from_pem(&self.ca_key)?)?;
        let key = KeyPair::from_pem(&self.server_key)?;
        let mut parameters = server_parameters(&self.hostname(), now)?;
        parameters.not_after = parameters
            .not_after
            .min(OffsetDateTime::from_unix_timestamp(self.ca_expires_at)?);
        self.certificate = parameters.signed_by(&key, &issuer)?.pem();
        self.expires_at = parameters.not_after.unix_timestamp();
        self.certificate_has_aki = true;
        Ok(true)
    }

    pub fn sign_device_request(&self, device_id: &str, csr: &str, now: i64) -> Result<String> {
        ensure!(
            crate::pairing::valid_identity(device_id)
                && csr.len() <= 4096
                && now >= 1704067200
                && now < self.ca_expires_at,
            "invalid device certificate request"
        );
        let mut request = CertificateSigningRequestParams::from_pem(csr)?;
        let hostname = format!("pg-device-{device_id}.local");
        let expected = SanType::DnsName(hostname.clone().try_into()?);
        ensure!(
            request.params.subject_alt_names == vec![expected],
            "CSR device name mismatch"
        );
        request.params = server_parameters(&hostname, now)?;
        request.params.not_after = request
            .params
            .not_after
            .min(OffsetDateTime::from_unix_timestamp(self.ca_expires_at)?);
        let issuer =
            Issuer::from_ca_cert_pem(&self.ca_certificate, KeyPair::from_pem(&self.ca_key)?)?;
        Ok(request.signed_by(&issuer)?.pem())
    }
}

fn server_parameters(hostname: &str, now: i64) -> Result<CertificateParams> {
    let mut parameters = CertificateParams::new(vec![hostname.to_owned()])?;
    parameters.distinguished_name = DistinguishedName::new();
    parameters
        .distinguished_name
        .push(DnType::CommonName, hostname);
    parameters.not_before = OffsetDateTime::from_unix_timestamp(now)? - Duration::minutes(5);
    parameters.not_after = OffsetDateTime::from_unix_timestamp(now)? + Duration::days(365);
    parameters.key_usages = vec![KeyUsagePurpose::DigitalSignature];
    parameters.extended_key_usages = vec![ExtendedKeyUsagePurpose::ServerAuth];
    parameters.use_authority_key_identifier_extension = true;
    Ok(parameters)
}

#[cfg(test)]
mod tests {
    use super::*;
    const NOW: i64 = 1793142000;

    #[test]
    fn previous_certificate_is_reissued_once_without_replacing_ca_or_keys() {
        let mut identity = EdgeIdentity::generate(NOW).unwrap();
        let mut parameters = server_parameters(&identity.hostname(), NOW).unwrap();
        parameters.use_authority_key_identifier_extension = false;
        let key = KeyPair::from_pem(&identity.server_key).unwrap();
        let issuer = Issuer::from_ca_cert_pem(
            &identity.ca_certificate,
            KeyPair::from_pem(&identity.ca_key).unwrap(),
        )
        .unwrap();
        identity.certificate = parameters.signed_by(&key, &issuer).unwrap().pem();
        let mut saved = serde_json::to_value(&identity).unwrap();
        saved.as_object_mut().unwrap().remove("certificate_has_aki");
        let mut restored: EdgeIdentity = serde_json::from_value(saved).unwrap();
        assert!(restored.renew(NOW).unwrap());
        assert_ne!(restored.certificate, identity.certificate);
        assert_eq!(restored.id, identity.id);
        assert_eq!(restored.ca_certificate, identity.ca_certificate);
        assert_eq!(restored.ca_key, identity.ca_key);
        assert_eq!(restored.server_key, identity.server_key);
        assert!(!restored.renew(NOW).unwrap());
    }

    #[test]
    fn renewal_preserves_identity_ca_and_server_key_and_device_csr_is_bound_to_name() {
        let mut identity = EdgeIdentity::generate(NOW).unwrap();
        let ca = identity.ca_certificate.clone();
        let key = identity.server_key.clone();
        let hostname = identity.hostname();
        assert!(!identity.renew(NOW + 274 * 86400).unwrap());
        assert!(identity.renew(NOW + 275 * 86400).unwrap());
        assert_eq!(identity.ca_certificate, ca);
        assert_eq!(identity.server_key, key);
        assert_eq!(identity.hostname(), hostname);
        let id = "2".repeat(32);
        let request = CertificateParams::new(vec![format!("pg-device-{id}.local")])
            .unwrap()
            .serialize_request(&KeyPair::generate().unwrap())
            .unwrap()
            .pem()
            .unwrap();
        assert!(identity.sign_device_request(&id, &request, NOW).is_ok());
        assert!(
            identity
                .sign_device_request(&"3".repeat(32), &request, NOW)
                .is_err()
        );
        assert!(identity.renew(NOW + 3650 * 86400).is_err());
    }
}
