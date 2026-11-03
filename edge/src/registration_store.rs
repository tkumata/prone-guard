use crate::registration::{Record, Registration};
use crate::{
    config::Device,
    pairing::{valid_identity, valid_secret},
};
use anyhow::{Result, ensure};
use rusqlite::{Connection, OptionalExtension, params};
use sha2::{Digest, Sha256};
use std::{
    fs::OpenOptions,
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::Path,
};

pub struct RegistrationStore {
    connection: Connection,
    _lock: std::fs::File,
}

impl RegistrationStore {
    pub fn open(path: &Path) -> Result<Self> {
        let mut lock_path = path.as_os_str().to_os_string();
        lock_path.push(".lock");
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(lock_path)?;
        ensure!(
            lock.metadata()?.is_file() && lock.metadata()?.permissions().mode() & 0o077 == 0,
            "registration lock must be private"
        );
        lock.try_lock().map_err(|_| {
            anyhow::anyhow!("registration store is already in use or cannot be locked")
        })?;
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(path)?;
        ensure!(
            file.metadata()?.is_file() && file.metadata()?.permissions().mode() & 0o077 == 0,
            "registration store must be private"
        );
        drop(file);
        let connection = Connection::open(path)?;
        connection.execute_batch("PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; CREATE TABLE IF NOT EXISTS registrations(device_id TEXT PRIMARY KEY, data TEXT NOT NULL); CREATE TABLE IF NOT EXISTS registration_transactions(transaction_id TEXT PRIMARY KEY, token_sha256 TEXT NOT NULL UNIQUE);")?;
        let store = Self {
            connection,
            _lock: lock,
        };
        store.connection.execute_batch("CREATE TABLE IF NOT EXISTS registration_metadata(name TEXT PRIMARY KEY, data TEXT NOT NULL)")?;
        for (id, record) in store.records()? {
            ensure!(valid_identity(&id), "invalid saved registration identity");
            for registration in record.active.iter().chain(record.pending.iter()) {
                registration.validate()?;
                ensure!(
                    registration.device_id == id && registration.generation <= record.generation,
                    "invalid saved registration"
                );
            }
        }
        Ok(store)
    }

    fn records(&self) -> Result<Vec<(String, Record)>> {
        let mut statement = self
            .connection
            .prepare("SELECT device_id,data FROM registrations")?;
        let values = statement.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })?;
        values
            .map(|row| {
                let (id, value) = row?;
                Ok((id, serde_json::from_str(&value)?))
            })
            .collect()
    }

    fn record(&self, id: &str) -> Result<Record> {
        ensure!(valid_identity(id), "invalid device identity");
        let raw = self
            .connection
            .query_row(
                "SELECT data FROM registrations WHERE device_id=?1",
                [id],
                |row| row.get::<_, String>(0),
            )
            .optional()?;
        Ok(match raw {
            Some(raw) => serde_json::from_str(&raw)?,
            None => Record::default(),
        })
    }

    fn save(&mut self, id: &str, record: &Record) -> Result<()> {
        let data = serde_json::to_string(record)?;
        let transaction = self.connection.transaction()?;
        transaction.execute("INSERT INTO registrations(device_id,data) VALUES(?1,?2) ON CONFLICT(device_id) DO UPDATE SET data=excluded.data", params![id,data])?;
        transaction.commit()?;
        Ok(())
    }

    pub fn next_generation(&self, id: &str) -> Result<u32> {
        self.record(id)?
            .generation
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("registration generation exhausted"))
    }

    pub fn edge_identity(&mut self, now: i64) -> Result<crate::identity::EdgeIdentity> {
        let saved = self
            .connection
            .query_row(
                "SELECT data FROM registration_metadata WHERE name='identity'",
                [],
                |row| row.get::<_, String>(0),
            )
            .optional()?;
        let mut identity: crate::identity::EdgeIdentity = match &saved {
            Some(raw) => serde_json::from_str(raw)?,
            None => crate::identity::EdgeIdentity::generate(now)?,
        };
        let renewed = identity.renew(now)?;
        if saved.is_none() || renewed {
            let raw = zeroize::Zeroizing::new(serde_json::to_string(&identity)?);
            let transaction = self.connection.transaction()?;
            transaction.execute("INSERT INTO registration_metadata(name,data) VALUES('identity',?1) ON CONFLICT(name) DO UPDATE SET data=excluded.data", [raw.as_str()])?;
            transaction.commit()?;
        }
        Ok(identity)
    }

    pub fn prepare(&mut self, registration: Registration) -> Result<()> {
        let id = registration.device_id.clone();
        let mut record = self.record(&id)?;
        if !record.prepare(registration, &self.records()?)? {
            return Ok(());
        }
        let data = serde_json::to_string(&record)?;
        let pending = record.pending.as_ref().unwrap();
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "INSERT INTO registration_transactions(transaction_id,token_sha256) VALUES(?1,?2)",
            params![pending.transaction_id, pending.token_sha256],
        )?;
        transaction.execute("INSERT INTO registrations(device_id,data) VALUES(?1,?2) ON CONFLICT(device_id) DO UPDATE SET data=excluded.data",params![id,data])?;
        transaction.commit()?;
        Ok(())
    }

    pub fn pending_matches(
        &self,
        authorization: &str,
        device_id: &str,
        edge_id: &str,
        transaction_id: &str,
        generation: u32,
    ) -> Result<bool> {
        if !valid_identity(device_id)
            || !valid_identity(edge_id)
            || !valid_identity(transaction_id)
            || generation == 0
        {
            return Ok(false);
        }
        let Some(hash) = key_hash(authorization) else {
            return Ok(false);
        };
        let record = self.record(device_id)?;
        Ok(record.pending_matches(&hash, edge_id, transaction_id, generation))
    }

    pub fn check(
        &mut self,
        authorization: &str,
        device_id: &str,
        edge_id: &str,
        transaction_id: &str,
        generation: u32,
    ) -> Result<()> {
        let hash = key_hash(authorization).ok_or_else(|| anyhow::anyhow!("invalid pairing key"))?;
        let mut record = self.record(device_id)?;
        record.check(&hash, edge_id, transaction_id, generation)?;
        self.save(device_id, &record)
    }

    pub fn device_committed(
        &mut self,
        id: &str,
        transaction_id: &str,
        generation: u32,
    ) -> Result<()> {
        let mut record = self.record(id)?;
        if record.device_committed(transaction_id, generation)? {
            self.save(id, &record)?;
        }
        Ok(())
    }

    pub fn activate(&mut self, id: &str, transaction_id: &str) -> Result<()> {
        let mut record = self.record(id)?;
        if record.activate(transaction_id)? {
            self.save(id, &record)?;
        }
        Ok(())
    }

    pub fn cancel(&mut self, id: &str, transaction_id: &str) -> Result<()> {
        let mut record = self.record(id)?;
        if record.cancel(transaction_id)? {
            self.save(id, &record)?;
        }
        Ok(())
    }

    pub fn revoke(&mut self, id: &str) -> Result<()> {
        let mut record = self.record(id)?;
        if record.revoke()? {
            self.save(id, &record)?;
        }
        Ok(())
    }

    pub fn authenticate(&self, authorization: &str) -> Result<Option<Device>> {
        let Some(hash) = key_hash(authorization) else {
            return Ok(None);
        };
        Ok(self
            .records()?
            .into_iter()
            .filter(|(_, r)| !r.revoked)
            .filter_map(|(_, r)| r.active)
            .find(|r| r.matches_hash(&hash))
            .map(|r| r.device()))
    }

    pub fn devices(&self) -> Result<Vec<Device>> {
        Ok(self
            .records()?
            .into_iter()
            .filter(|(_, r)| !r.revoked)
            .filter_map(|(_, r)| r.active)
            .map(|r| r.device())
            .collect())
    }

    pub fn status(&self, id: &str) -> Result<serde_json::Value> {
        let record = self.record(id)?;
        Ok(
            serde_json::json!({"device_id":id,"generation":record.generation,"active":record.active.as_ref().map(|r| serde_json::json!({"bed_id":r.bed_id,"transaction_id":r.transaction_id,"generation":r.generation})),"pending":record.pending.as_ref().map(|r| serde_json::json!({"bed_id":r.bed_id,"transaction_id":r.transaction_id,"generation":r.generation})),"checked":record.checked,"device_committed":record.device_committed,"revoked":record.revoked}),
        )
    }
}

fn key_hash(authorization: &str) -> Option<String> {
    let key = authorization.strip_prefix("Bearer ")?;
    if !valid_secret(key) {
        return None;
    }
    Some(
        Sha256::digest(key.as_bytes())
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pending_keys_require_check_and_device_commit_and_revocation_survives_restart() {
        let path = std::env::temp_dir().join(format!(
            "prone-registration-{}.sqlite",
            crate::security2::random_hex(8).unwrap()
        ));
        let id = "1".repeat(32);
        let edge = "2".repeat(32);
        let tx = "3".repeat(32);
        let authorization = format!("Bearer {}", "a".repeat(64));
        let registration = Registration {
            device_id: id.clone(),
            edge_id: edge.clone(),
            transaction_id: tx.clone(),
            generation: 1,
            bed_id: "bed-01".into(),
            token_sha256: key_hash(&authorization).unwrap(),
            installation: serde_json::json!({}),
        };
        let mut store = RegistrationStore::open(&path).unwrap();
        assert!(RegistrationStore::open(&path).is_err());
        let identity = store.edge_identity(crate::now()).unwrap();
        let identity_id = identity.id.clone();
        let identity_ca = identity.ca_certificate.clone();
        let identity_certificate = identity.certificate.clone();
        store.prepare(registration.clone()).unwrap();
        store.prepare(registration).unwrap();
        assert!(store.authenticate(&authorization).unwrap().is_none());
        assert!(store.activate(&id, &tx).is_err());
        assert!(
            store
                .check(&authorization, &id, &edge, &"4".repeat(32), 1)
                .is_err()
        );
        store
            .connection
            .pragma_update(None, "query_only", true)
            .unwrap();
        assert!(store.check(&authorization, &id, &edge, &tx, 1).is_err());
        assert_eq!(store.status(&id).unwrap()["checked"], false);
        store
            .connection
            .pragma_update(None, "query_only", false)
            .unwrap();
        store.check(&authorization, &id, &edge, &tx, 1).unwrap();
        assert!(store.authenticate(&authorization).unwrap().is_none());
        store.device_committed(&id, &tx, 1).unwrap();
        assert!(store.cancel(&id, &tx).is_err());
        drop(store);
        let mut store = RegistrationStore::open(&path).unwrap();
        let reopened = store.edge_identity(crate::now()).unwrap();
        assert_eq!(reopened.id, identity_id);
        assert_eq!(reopened.ca_certificate, identity_ca);
        assert_eq!(reopened.certificate, identity_certificate);
        store.activate(&id, &tx).unwrap();
        store.activate(&id, &tx).unwrap();
        assert!(store.authenticate(&authorization).unwrap().is_some());
        assert!(!store.status(&id).unwrap().to_string().contains("token"));
        store.revoke(&id).unwrap();
        drop(store);
        let store = RegistrationStore::open(&path).unwrap();
        assert!(store.authenticate(&authorization).unwrap().is_none());
        assert_eq!(store.next_generation(&id).unwrap(), 2);
        drop(store);
        std::fs::remove_file(format!("{}.lock", path.display())).unwrap();
        std::fs::remove_file(path).unwrap();
    }
}
