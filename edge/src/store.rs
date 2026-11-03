use crate::measurement::{Measurement, Received};
use anyhow::{Context, Result, bail, ensure};
use rusqlite::{Connection, ErrorCode, OptionalExtension, params};
use std::{
    fs,
    path::{Path, PathBuf},
    time::Instant,
};

pub const CAPACITY: u64 = 100_000_000;

pub struct Store {
    connection: Connection,
    path: PathBuf,
    retention: i64,
    capacity: u64,
    pub writable: bool,
    retention_clock: Option<(i64, Instant)>,
}

impl Store {
    pub fn open(path: &Path, retention: i64, capacity: u64) -> Result<Self> {
        use std::os::unix::fs::OpenOptionsExt;
        fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(path)?;
        crate::config::private_file(path)?;
        let connection = Connection::open(path)?;
        connection.execute_batch("PRAGMA page_size=4096; PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY; PRAGMA secure_delete=ON; PRAGMA busy_timeout=2000;")?;
        let page_size: i64 = connection.query_row("PRAGMA page_size", [], |r| r.get(0))?;
        ensure!(page_size == 4096, "unsupported database page size");
        // Reserve space for a full rollback journal, its headers, and filesystem rounding.
        let pages = (capacity * 48 / 100 / 4096) as i64;
        ensure!(pages >= 16, "storage budget too small");
        let size: i64 = connection.query_row("PRAGMA page_count", [], |r| r.get(0))?;
        ensure!(size <= pages, "existing database exceeds storage budget");
        let actual: i64 =
            connection.query_row(&format!("PRAGMA max_page_count={pages}"), [], |r| r.get(0))?;
        ensure!(actual == pages, "could not enforce database capacity");
        connection.execute_batch(
            "CREATE TABLE IF NOT EXISTS observations (
            bed TEXT NOT NULL, boot TEXT NOT NULL, sequence INTEGER NOT NULL,
            observed_at INTEGER NOT NULL, received_at INTEGER NOT NULL, retained_at INTEGER NOT NULL, json TEXT NOT NULL,
            PRIMARY KEY (bed, boot, sequence));
            CREATE INDEX IF NOT EXISTS observation_window ON observations(bed, observed_at);
            CREATE INDEX IF NOT EXISTS observation_expiry ON observations(retained_at);
            CREATE TABLE IF NOT EXISTS results (bed TEXT NOT NULL, window_end INTEGER NOT NULL,
            retained_at INTEGER NOT NULL, json TEXT NOT NULL, PRIMARY KEY(bed, window_end));",
        )?;
        let last:Option<i64>=connection.query_row("SELECT MAX(retained_at) FROM (SELECT retained_at FROM observations UNION ALL SELECT retained_at FROM results)",[],|r|r.get(0))?;
        let store = Self {
            connection,
            path: path.into(),
            retention,
            capacity,
            writable: true,
            retention_clock: last.map(|last| (last, Instant::now())),
        };
        store.check_size()?;
        Ok(store)
    }

    pub fn check_size(&self) -> Result<u64> {
        let mut bytes = 0;
        for suffix in ["", "-journal", "-wal", "-shm"] {
            let path = PathBuf::from(format!("{}{suffix}", self.path.display()));
            match fs::metadata(path) {
                Ok(metadata) => bytes += metadata.len(),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
        }
        ensure!(bytes <= self.capacity, "storage capacity exceeded");
        Ok(bytes)
    }

    fn retention_now(&mut self, wall: i64, monotonic: Instant) -> i64 {
        let (reference, started) = self.retention_clock.get_or_insert((wall, monotonic));
        let effective = *reference + monotonic.duration_since(*started).as_secs() as i64;
        if wall > effective {
            *reference = wall;
            *started = monotonic;
            wall
        } else {
            effective
        }
    }

    pub fn expire(&mut self, now: i64) -> Result<()> {
        let cutoff = self.retention_now(now, Instant::now()) - self.retention;
        let transaction = self.connection.transaction()?;
        transaction.execute("DELETE FROM observations WHERE retained_at<=?1", [cutoff])?;
        transaction.execute("DELETE FROM results WHERE retained_at<=?1", [cutoff])?;
        transaction.commit()?;
        self.check_size()?;
        Ok(())
    }

    fn evict(&mut self) -> Result<()> {
        let pages: i64 = self
            .connection
            .query_row("PRAGMA page_count", [], |r| r.get(0))?;
        let free: i64 = self
            .connection
            .query_row("PRAGMA freelist_count", [], |r| r.get(0))?;
        let limit = (self.capacity * 48 / 100 / 4096) as i64;
        ensure!(
            pages >= limit - 16 && free < 16,
            "disk full outside the configured data budget"
        );
        let transaction = self.connection.transaction()?;
        let count = transaction.execute("DELETE FROM observations WHERE rowid IN (SELECT rowid FROM observations ORDER BY retained_at, rowid LIMIT 64)", [])?;
        let results = transaction.execute("DELETE FROM results WHERE rowid IN (SELECT rowid FROM results ORDER BY retained_at, rowid LIMIT 8)", [])?;
        ensure!(count + results > 0, "storage exhausted");
        transaction.commit()?;
        eprintln!(
            "{}",
            serde_json::json!({"event":"capacity_eviction","observations":count,"results":results})
        );
        Ok(())
    }

    pub fn insert(&mut self, measurement: &Measurement, raw: &str, now: i64) -> Result<()> {
        let result = self.insert_inner(measurement, raw, now);
        if result
            .as_ref()
            .err()
            .is_none_or(|error| error.to_string() != "sequence_conflict")
        {
            self.writable = result.is_ok();
        }
        result
    }

    fn insert_inner(&mut self, m: &Measurement, raw: &str, now: i64) -> Result<()> {
        self.expire(now)?;
        let retained_at = self.retention_now(now, Instant::now());
        let existing: Option<String> = self
            .connection
            .query_row(
                "SELECT json FROM observations WHERE bed=?1 AND boot=?2 AND sequence=?3",
                params![m.bed_id, m.boot_id, m.sequence],
                |r| r.get(0),
            )
            .optional()?;
        if let Some(existing) = existing {
            ensure!(
                serde_json::from_str::<serde_json::Value>(&existing)?
                    == serde_json::from_str::<serde_json::Value>(raw)?,
                "sequence_conflict"
            );
            return Ok(());
        }
        loop {
            let result = self.connection.execute(
                "INSERT INTO observations VALUES (?1,?2,?3,?4,?5,?6,?7)",
                params![
                    m.bed_id,
                    m.boot_id,
                    m.sequence,
                    m.timestamp,
                    now,
                    retained_at,
                    raw
                ],
            );
            match result {
                Ok(_) => break,
                Err(rusqlite::Error::SqliteFailure(error, _))
                    if error.code == ErrorCode::DiskFull =>
                {
                    self.evict()?
                }
                Err(error) => return Err(error.into()),
            }
        }
        self.check_size()?;
        Ok(())
    }

    pub fn window(&mut self, bed: &str, end: i64, now: i64) -> Result<Vec<Received>> {
        let cutoff = self.retention_now(now, Instant::now()) - self.retention;
        let mut statement = self.connection.prepare("SELECT json, received_at FROM observations WHERE rowid IN (SELECT MAX(rowid) FROM observations WHERE bed=?1 AND observed_at>=?2 AND observed_at<?3 AND retained_at>?4 GROUP BY observed_at) ORDER BY observed_at")?;
        let rows = statement.query_map(params![bed, end - 180, end, cutoff], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?))
        })?;
        rows.map(|row| {
            let (json, received_at) = row?;
            Ok(Received {
                measurement: serde_json::from_str(&json).context("corrupt stored observation")?,
                received_at,
            })
        })
        .collect()
    }

    pub fn result(
        &mut self,
        bed: &str,
        end: i64,
        now: i64,
        result: &serde_json::Value,
    ) -> Result<()> {
        let json = serde_json::to_string(result)?;
        ensure!(json.len() <= 16384, "result exceeds storage limit");
        let retained_at = self.retention_now(now, Instant::now());
        loop {
            match self.connection.execute(
                "INSERT OR REPLACE INTO results VALUES (?1,?2,?3,?4)",
                params![bed, end, retained_at, json],
            ) {
                Ok(_) => break,
                Err(rusqlite::Error::SqliteFailure(error, _))
                    if error.code == ErrorCode::DiskFull =>
                {
                    self.evict()?
                }
                Err(error) => bail!(error),
            }
        }
        self.check_size()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clock_rollback_does_not_extend_retention() {
        let path = std::env::temp_dir().join(format!("prone-clock-{}.sqlite", std::process::id()));
        let mut store = Store::open(&path, 21600, CAPACITY).unwrap();
        let start = Instant::now();
        assert_eq!(store.retention_now(100000, start), 100000);
        assert_eq!(
            store.retention_now(90000, start + std::time::Duration::from_secs(21600)),
            121600
        );
        drop(store);
        fs::remove_file(path).unwrap();
    }
}
