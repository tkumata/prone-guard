use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::{
    env, fs,
    net::SocketAddr,
    path::{Path, PathBuf},
};
use subtle::ConstantTimeEq;

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Device {
    pub bed_id: String,
    pub token_sha256: String,
    pub installation: serde_json::Value,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub listen: SocketAddr,
    pub database: PathBuf,
    #[serde(default)]
    pub tls_certificate: PathBuf,
    #[serde(default)]
    pub tls_private_key: PathBuf,
    #[serde(default)]
    pub registrations: Option<PathBuf>,
    #[serde(default = "retention_default")]
    pub retention_seconds: i64,
    pub devices: Vec<Device>,
}

fn retention_default() -> i64 {
    6 * 60 * 60
}

pub fn private_file(path: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    ensure!(
        fs::metadata(path)?.permissions().mode() & 0o077 == 0,
        "secret file must have mode 0600: {}",
        path.display()
    );
    Ok(())
}

impl Config {
    pub fn load(path: &Path) -> Result<Self> {
        let config: Self =
            serde_json::from_slice(&fs::read(path)?).context("invalid configuration")?;
        ensure!(
            config.retention_seconds >= 180 && config.retention_seconds <= 365 * 86400,
            "invalid retention period"
        );
        ensure!(
            ((!config.devices.is_empty() && config.registrations.is_none())
                || (config.devices.is_empty() && config.registrations.is_some()))
                && config.devices.len() <= 64,
            "configure 1..64 devices"
        );
        let mut beds = std::collections::HashSet::new();
        let mut keys = std::collections::HashSet::new();
        for device in &config.devices {
            ensure!(
                crate::measurement::valid_id(&device.bed_id) && beds.insert(&device.bed_id),
                "invalid or duplicate bed identifier"
            );
            ensure!(
                device.token_sha256.len() == 64
                    && device
                        .token_sha256
                        .bytes()
                        .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
                    && keys.insert(&device.token_sha256),
                "invalid or duplicate device key hash"
            );
            ensure!(
                device.installation.is_object()
                    && serde_json::to_vec(&device.installation)?.len() <= 4096,
                "invalid installation description"
            );
        }
        if config.registrations.is_none() {
            private_file(&config.tls_private_key)?;
        }
        Ok(config)
    }

    pub fn authenticate(&self, authorization: &str) -> Option<&Device> {
        let token = authorization.strip_prefix("Bearer ")?;
        if token.len() != 64
            || !token
                .bytes()
                .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
        {
            return None;
        }
        let hash = Sha256::digest(token.as_bytes())
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>();
        self.devices
            .iter()
            .find(|device| bool::from(device.token_sha256.as_bytes().ct_eq(hash.as_bytes())))
    }
}

pub fn api_key() -> Result<Option<String>> {
    let key = match env::var("JEV_API_KEY") {
        Ok(key) => key,
        Err(env::VarError::NotPresent) => return Ok(None),
        Err(error) => return Err(error).context("invalid JEV_API_KEY encoding"),
    };
    ensure!(
        !key.is_empty() && key.len() <= 512 && key.bytes().all(|b| (33..=126).contains(&b)),
        "invalid JEV_API_KEY"
    );
    Ok(Some(key))
}
