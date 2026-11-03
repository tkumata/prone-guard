use anyhow::{Context, Result};
use prone_guard_edge::{App, config, discovery::Discovery, maintenance, router, scheduler, store};
use std::{
    env,
    path::Path,
    sync::{Arc, Mutex, atomic::AtomicBool},
};

#[tokio::main]
async fn main() -> Result<()> {
    let path = env::args()
        .nth(1)
        .context("usage: prone-guard-edge CONFIG.json")?;
    let config = config::Config::load(Path::new(&path))?;
    let key = config::api_key()?;
    let address = config.listen;
    let store = store::Store::open(&config.database, config.retention_seconds, store::CAPACITY)?;
    let mut registrations = config
        .registrations
        .as_ref()
        .map(|path| {
            prone_guard_edge::registration_store::RegistrationStore::open(path).map(Mutex::new)
        })
        .transpose()?;
    let (tls, discovery) = if let Some(registrations) = &mut registrations {
        let identity = registrations
            .get_mut()
            .map_err(|_| anyhow::anyhow!("registration lock poisoned"))?
            .edge_identity(prone_guard_edge::now())?;
        let (certificate, key) = identity.tls_pem();
        (
            axum_server::tls_rustls::RustlsConfig::from_pem(certificate, key).await?,
            Some(Discovery::advertise(&identity.id, address.port())?),
        )
    } else {
        (
            axum_server::tls_rustls::RustlsConfig::from_pem_file(
                &config.tls_certificate,
                &config.tls_private_key,
            )
            .await?,
            None,
        )
    };
    let app = Arc::new(App {
        config,
        store: Mutex::new(store),
        registrations,
        jev_available: AtomicBool::new(false),
    });
    let scheduler = tokio::spawn(scheduler(app.clone(), key));
    let maintenance = tokio::spawn(maintenance(app.clone()));
    let certificate_maintenance = tokio::spawn(renew_certificates(app.clone(), tls.clone()));
    let discovery_monitor = async {
        match &discovery {
            Some(discovery) => discovery.monitor().await,
            None => std::future::pending::<Result<()>>().await,
        }
    };
    tokio::select! {
        result=axum_server::bind_rustls(address,tls).serve(router(app).into_make_service()) => {result?;}
        result=scheduler => {result??; anyhow::bail!("scheduler stopped");}
        result=maintenance => {result??; anyhow::bail!("maintenance stopped");}
        result=certificate_maintenance => {result??; anyhow::bail!("certificate maintenance stopped");}
        result=discovery_monitor => {result?; anyhow::bail!("discovery stopped");}
        result=tokio::signal::ctrl_c() => {result?;}
    }
    Ok(())
}

async fn renew_certificates(
    app: Arc<App>,
    tls: axum_server::tls_rustls::RustlsConfig,
) -> Result<()> {
    let mut ca_warning = false;
    loop {
        tokio::time::sleep(std::time::Duration::from_secs(3600)).await;
        let identity = tokio::task::spawn_blocking({
            let app = app.clone();
            move || -> Result<_> {
                app.registrations
                    .as_ref()
                    .map(|store| {
                        store
                            .lock()
                            .map_err(|_| anyhow::anyhow!("registration lock poisoned"))?
                            .edge_identity(prone_guard_edge::now())
                    })
                    .transpose()
            }
        })
        .await??;
        if let Some(identity) = identity {
            let (certificate, key) = identity.tls_pem();
            tls.reload_from_pem(certificate, key).await?;
            if !ca_warning && identity.ca_expires_at - prone_guard_edge::now() <= 365 * 86400 {
                eprintln!(
                    "{}",
                    serde_json::json!({"event":"ca_replacement_required","recovery":"authenticated_usb"})
                );
                ca_warning = true;
            }
        }
    }
}
