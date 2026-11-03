use crate::{
    config::Device,
    pairing::{valid_identity, valid_secret},
};
use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};
use subtle::ConstantTimeEq;

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Registration {
    pub device_id: String,
    pub edge_id: String,
    pub transaction_id: String,
    pub generation: u32,
    pub bed_id: String,
    pub token_sha256: String,
    pub installation: serde_json::Value,
}

impl Registration {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            valid_identity(&self.device_id)
                && valid_identity(&self.edge_id)
                && valid_identity(&self.transaction_id),
            "invalid registration identity"
        );
        ensure!(
            self.generation > 0
                && !self.bed_id.is_empty()
                && crate::measurement::valid_id(&self.bed_id),
            "invalid registration generation or bed"
        );
        ensure!(
            valid_secret(&self.token_sha256),
            "invalid registration key hash"
        );
        ensure!(
            self.installation.is_object() && serde_json::to_vec(&self.installation)?.len() <= 4096,
            "invalid installation description"
        );
        Ok(())
    }

    pub(crate) fn device(&self) -> Device {
        Device {
            bed_id: self.bed_id.clone(),
            token_sha256: self.token_sha256.clone(),
            installation: self.installation.clone(),
        }
    }
}

#[derive(Clone, Default, Deserialize, Serialize)]
pub(crate) struct Record {
    pub(crate) generation: u32,
    pub(crate) active: Option<Registration>,
    pub(crate) pending: Option<Registration>,
    pub(crate) checked: bool,
    pub(crate) device_committed: bool,
    pub(crate) revoked: bool,
    pub(crate) last_cancelled: Option<String>,
}

impl Registration {
    pub(crate) fn matches_hash(&self, hash: &str) -> bool {
        bool::from(hash.as_bytes().ct_eq(self.token_sha256.as_bytes()))
    }
}

impl Record {
    pub(crate) fn prepare(
        &mut self,
        registration: Registration,
        records: &[(String, Record)],
    ) -> Result<bool> {
        registration.validate()?;
        if let Some(pending) = &self.pending {
            ensure!(
                serde_json::to_value(pending)? == serde_json::to_value(&registration)?,
                "another registration is pending"
            );
            return Ok(false);
        }
        ensure!(
            self.active
                .as_ref()
                .is_none_or(|r| r.transaction_id != registration.transaction_id)
                && self.last_cancelled.as_deref() != Some(&registration.transaction_id),
            "transaction already finalized"
        );
        ensure!(
            self.generation.checked_add(1) == Some(registration.generation),
            "registration generation mismatch"
        );
        let occupied = records
            .iter()
            .filter(|(_, r)| r.pending.is_some() || (r.active.is_some() && !r.revoked))
            .count();
        ensure!(
            occupied < 64 || (self.active.is_some() && !self.revoked),
            "device capacity exceeded"
        );
        for (id, other) in records {
            for existing in other
                .pending
                .iter()
                .chain(other.active.iter().filter(|_| !other.revoked))
            {
                ensure!(
                    *id == registration.device_id || existing.bed_id != registration.bed_id,
                    "bed is already registered"
                );
                ensure!(
                    existing.token_sha256 != registration.token_sha256,
                    "registration key reused"
                );
            }
        }
        self.generation = registration.generation;
        self.pending = Some(registration);
        self.checked = false;
        self.device_committed = false;
        Ok(true)
    }

    pub(crate) fn pending_matches(
        &self,
        hash: &str,
        edge_id: &str,
        transaction_id: &str,
        generation: u32,
    ) -> bool {
        self.pending.as_ref().is_some_and(|pending| {
            pending.edge_id == edge_id
                && pending.transaction_id == transaction_id
                && pending.generation == generation
                && pending.matches_hash(hash)
        })
    }

    pub(crate) fn check(
        &mut self,
        hash: &str,
        edge_id: &str,
        transaction_id: &str,
        generation: u32,
    ) -> Result<()> {
        ensure!(
            self.pending_matches(hash, edge_id, transaction_id, generation),
            "pairing identity or key mismatch"
        );
        self.checked = true;
        Ok(())
    }

    pub(crate) fn device_committed(
        &mut self,
        transaction_id: &str,
        generation: u32,
    ) -> Result<bool> {
        if self.pending.is_none() {
            ensure!(
                !self.revoked
                    && self.active.as_ref().is_some_and(
                        |r| r.transaction_id == transaction_id && r.generation == generation
                    ),
                "registration transaction mismatch"
            );
            return Ok(false);
        }
        ensure!(
            self.checked
                && self.pending.as_ref().is_some_and(
                    |r| r.transaction_id == transaction_id && r.generation == generation
                ),
            "registration not checked or mismatched"
        );
        let changed = !self.device_committed;
        self.device_committed = true;
        Ok(changed)
    }

    pub(crate) fn activate(&mut self, transaction_id: &str) -> Result<bool> {
        if self.pending.is_none() {
            ensure!(
                !self.revoked
                    && self
                        .active
                        .as_ref()
                        .is_some_and(|r| r.transaction_id == transaction_id),
                "registration transaction mismatch"
            );
            return Ok(false);
        }
        ensure!(
            self.checked
                && self.device_committed
                && self
                    .pending
                    .as_ref()
                    .is_some_and(|r| r.transaction_id == transaction_id),
            "device commit not confirmed"
        );
        self.active = self.pending.take();
        self.revoked = false;
        self.checked = false;
        self.device_committed = false;
        Ok(true)
    }

    pub(crate) fn cancel(&mut self, transaction_id: &str) -> Result<bool> {
        if self.pending.is_none() && self.last_cancelled.as_deref() == Some(transaction_id) {
            return Ok(false);
        }
        ensure!(
            !self.device_committed
                && self
                    .pending
                    .as_ref()
                    .is_some_and(|r| r.transaction_id == transaction_id),
            "cannot cancel mismatched or device-committed transaction"
        );
        self.pending = None;
        self.checked = false;
        self.last_cancelled = Some(transaction_id.to_owned());
        Ok(true)
    }

    pub(crate) fn revoke(&mut self) -> Result<bool> {
        ensure!(
            self.active.is_some() || self.pending.is_some(),
            "unknown device"
        );
        self.revoked = true;
        if let Some(pending) = self.pending.take() {
            self.last_cancelled = Some(pending.transaction_id);
        }
        self.checked = false;
        self.device_committed = false;
        Ok(true)
    }
}
