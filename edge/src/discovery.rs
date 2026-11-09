use anyhow::{Result, ensure};
use mdns_sd::{DaemonEvent, Receiver, ServiceDaemon, ServiceInfo};
use std::time::Duration;

pub struct Discovery {
    daemon: ServiceDaemon,
    events: Receiver<DaemonEvent>,
    fullname: String,
}

impl Discovery {
    pub fn advertise(id: &str, port: u16) -> Result<Self> {
        let service = service(id, port)?;
        let fullname = service.get_fullname().to_owned();
        let daemon = ServiceDaemon::new()?;
        let events = daemon.monitor()?;
        let discovery = Self {
            daemon,
            events,
            fullname,
        };
        discovery.daemon.register(service)?;
        Ok(discovery)
    }

    pub async fn monitor(self) {
        let reason = loop {
            match self.events.recv_async().await {
                Ok(DaemonEvent::NameChange(_)) => {
                    break "mDNS name conflict; original TLS identity retained";
                }
                Ok(DaemonEvent::Error(_)) => break "mDNS advertisement failed",
                Ok(_) => {}
                Err(_) => break "mDNS daemon stopped",
            }
        };
        eprintln!(
            "{}",
            serde_json::json!({"event":"mdns_advertisement_failed","reason":reason})
        );
        // Dropping Discovery waits for the daemon, so do not block a runtime worker.
        tokio::task::spawn_blocking(move || drop(self));
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
