use anyhow::{Context, Result, ensure};
use prone_guard_edge::{now, pairing::UsbSession};
use serde_json::json;
use zeroize::Zeroizing;

fn main() -> Result<()> {
    let path = std::env::args()
        .nth(1)
        .context("usage: pairing_probe USB_PORT DEVICE_ID")?;
    let id = std::env::args().nth(2).context("device ID required")?;
    let password = Zeroizing::new(
        std::env::var("PRONE_GUARD_TEST_CODE")
            .context("set PRONE_GUARD_TEST_CODE for test firmware")?,
    );
    let mut session = UsbSession::connect(&path, id.clone(), password)?;
    let identity = session.command(&json!({"op":"identity"}))?;
    ensure!(identity["device_id"] == id, "device identity mismatch");
    let utc = now();
    let clock = session.command(&json!({"op":"time","utc":utc}))?;
    ensure!(
        clock["utc"]
            .as_i64()
            .is_some_and(|value| (value - utc).abs() <= 5),
        "device clock not confirmed"
    );
    println!("Security 2 device authentication and authenticated UTC setting passed");
    Ok(())
}
