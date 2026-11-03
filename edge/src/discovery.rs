use anyhow::{Result, ensure};
use mdns_sd::{DaemonEvent, ServiceDaemon, ServiceInfo};
use std::time::Duration;

pub struct Discovery {
    daemon: ServiceDaemon,
    fullname: String,
}

impl Discovery {
    pub fn advertise(id: &str, port: u16) -> Result<Self> {
        let service = service(id, port)?;
        let fullname = service.get_fullname().to_owned();
        let daemon = ServiceDaemon::new()?;
        daemon.register(service)?;
        Ok(Self { daemon, fullname })
    }

    pub async fn monitor(&self) -> Result<()> {
        let monitor = self.daemon.monitor()?;
        while let Ok(event) = monitor.recv_async().await {
            match event {
                DaemonEvent::NameChange(_) => {
                    anyhow::bail!("mDNS name conflict; original TLS identity retained")
                }
                DaemonEvent::Error(_) => anyhow::bail!("mDNS advertisement failed"),
                _ => {}
            }
        }
        anyhow::bail!("mDNS daemon stopped")
    }
}

impl Drop for Discovery {
    fn drop(&mut self) {
        if let Ok(result) = self.daemon.unregister(&self.fullname) {
            let _ = result.recv_timeout(Duration::from_secs(1));
        }
        if let Ok(result) = self.daemon.shutdown() {
            let _ = result.recv_timeout(Duration::from_secs(1));
        }
    }
}

fn service(id: &str, port: u16) -> Result<ServiceInfo> {
    ensure!(
        crate::pairing::valid_identity(id) && port != 0,
        "invalid discovery identity or port"
    );
    Ok(ServiceInfo::new(
        "_prone-guard._tcp.local.",
        &format!("pg-edge-{id}"),
        &format!("pg-edge-{id}.local."),
        "",
        port,
        [("version", "1")].as_slice(),
    )?
    .enable_addr_auto())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn discovery_uses_the_stable_tls_name_without_credentials() {
        let id = "1".repeat(32);
        let service = service(&id, 8443).unwrap();
        assert_eq!(service.get_hostname(), format!("pg-edge-{id}.local."));
        assert_eq!(service.get_port(), 8443);
        assert_eq!(service.get_properties().len(), 1);
        assert!(super::service("../invalid", 8443).is_err());
        assert!(super::service(&id, 0).is_err());
    }
}
