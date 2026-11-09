use axum::{
    body::Body,
    http::{Request, StatusCode},
};
use prone_guard_edge::{
    App,
    config::{Config, Device},
    jev,
    measurement::{self, Measurement, Received},
    router,
    store::Store,
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};
use tower::ServiceExt;

const START: i64 = 1793142000;
fn sample(second: i64) -> Measurement {
    let mut m: Measurement =
        serde_json::from_str(include_str!("../../tests/fixtures/measurement.jsonl")).unwrap();
    m.timestamp = second;
    m.camera.timestamp = Some(second);
    m.depth.timestamp = Some(second);
    m.sequence = second;
    m
}
fn rows(end: i64) -> Vec<Received> {
    (end - 180..end)
        .map(|second| Received {
            measurement: sample(second),
            received_at: second,
        })
        .collect()
}
struct Database(PathBuf);
impl Database {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        Self(std::env::temp_dir().join(format!(
            "prone-edge-{}-{}.sqlite",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        )))
    }
}
impl Drop for Database {
    fn drop(&mut self) {
        for suffix in ["", "-journal", "-wal", "-shm", ".lock"] {
            let path = format!("{}{suffix}", self.0.display());
            match std::fs::remove_file(path) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => panic!("cleanup: {e}"),
            }
        }
    }
}

#[test]
fn firmware_fixture_and_untrusted_input() {
    let mut m = sample(START);
    m.validate(START).unwrap();
    m.camera.timestamp = Some(i64::MIN);
    assert!(m.validate(START).is_err());
    m = sample(START);
    m.camera.face_area_px2 = Some(-1.0);
    assert!(m.validate(START).is_err());
    m = sample(START);
    m.camera.face_detected = Some(0);
    assert!(m.validate(START).is_err());
    m = sample(START);
    m.timestamp += 6;
    assert!(m.validate(START).is_err());
    let mut value = serde_json::to_value(sample(START)).unwrap();
    value["depth"]["distance"] = json!(vec![vec![1000; 8]; 7]);
    assert!(serde_json::from_value::<Measurement>(value).is_err());
}

#[test]
fn missing_data_duplicates_and_staleness_do_not_pass_quality_gate() {
    let end = START + 180;
    let full = measurement::prepare_window(rows(end), end).unwrap();
    assert_eq!(full.valid_seconds, 180);
    let mut missing = rows(end);
    missing.retain(|r| r.measurement.timestamp % 10 != 0);
    assert_eq!(
        measurement::prepare_window(missing, end)
            .unwrap()
            .valid_seconds,
        162
    );
    let mut missing = rows(end);
    missing.drain(0..19);
    assert!(measurement::prepare_window(missing, end).is_err());
    let mut gap = rows(end);
    gap.drain(80..86);
    assert!(measurement::prepare_window(gap, end).is_err());
    let mut duplicates = rows(end);
    for r in &mut duplicates {
        r.measurement.timestamp = START;
    }
    assert!(measurement::prepare_window(duplicates, end).is_err());
    let mut late = rows(end);
    for r in &mut late {
        r.received_at += 6;
    }
    assert!(measurement::prepare_window(late, end).is_err());
    let mut invalid = rows(end);
    for r in &mut invalid {
        r.measurement.depth.target_status = Some([[6; 8]; 8]);
    }
    assert!(measurement::prepare_window(invalid, end).is_err());
    let mut absent = rows(end);
    for r in &mut absent {
        r.measurement.camera.face_detected = Some(0);
        r.measurement.camera.face_area_px2 = Some(0.0);
    }
    assert!(measurement::prepare_window(absent, end).is_ok());
}

#[test]
fn depth_motion_candidate_requires_multiple_valid_changed_cells() {
    let end = START + 180;
    let mut data = rows(end);
    assert!(jev::depth_change_candidates(&measurement::prepare_window(rows(end), end).unwrap()).is_empty());
    for row in data.iter_mut().skip(80) {
        let m = &mut row.measurement;
        let mut distance = m.depth.distance.unwrap();
        for y in 0..4 {
            for x in 0..4 {
                distance[y][x] += 200;
            }
        }
        m.depth.distance = Some(distance);
    }
    let window = measurement::prepare_window(data, end).unwrap();
    let candidates = jev::depth_change_candidates(&window);
    assert_eq!(candidates.len(), 1);
    assert_eq!(candidates[0]["start"], START + 79);
    assert_eq!(candidates[0]["end"], START + 80);
}

#[test]
fn optional_face_metadata_is_validated() {
    let mut m = sample(START);
    m.camera.face_confidence = Some(0.8);
    assert!(m.validate(START).is_err());
    m.camera.face_box = Some([10, 10, 20, 20]);
    m.camera.face_area_px2 = Some(100.0);
    m.validate(START).unwrap();
    m.camera.face_landmarks = Some([[10, 10]; 5]);
    m.validate(START).unwrap();
    m.camera.face_landmarks = Some([[400, 10]; 5]);
    assert!(m.validate(START).is_err());
}

#[test]
fn jev_rollover_request_contains_the_ordered_window_and_excludes_device_identity() {
    let window = measurement::prepare_window(rows(START + 180), START + 180).unwrap();
    let request = jev::request(&window, START + 180, &json!({"mount":"overhead"})).unwrap();
    let jsonl = request["state"]["observations_jsonl"].as_str().unwrap();
    assert_eq!(jsonl.lines().count(), 180);
    let expected = sample(START);
    for (second, line) in (START..START + 180).zip(jsonl.lines()) {
        let observation: Value = serde_json::from_str(line).unwrap();
        assert_eq!(observation["timestamp"], second);
        assert!(observation.get("bed_id").is_none() && observation.get("boot_id").is_none());
        assert_eq!(
            observation["camera"]["face_detected"],
            json!(expected.camera.face_detected)
        );
        assert_eq!(
            observation["camera"]["face_area_px2"],
            json!(expected.camera.face_area_px2)
        );
        assert_eq!(
            observation["depth"]["distance"],
            json!(expected.depth.distance)
        );
        assert_eq!(
            observation["depth"]["target_status"],
            json!(expected.depth.target_status)
        );
    }
    assert!(request["state"]["depth_change_candidates"].is_array());
    assert_eq!(request["state"]["window"]["start"], START);
    assert_eq!(request["state"]["window"]["end_exclusive"], START + 180);
    let questions = request["questions"].as_object().unwrap();
    assert_eq!(questions.len(), 1);
    let question = &questions["rollover"];
    assert_eq!(question["type"], "choice");
    let criteria = question["criteria"].as_object().unwrap();
    assert_eq!(criteria.len(), 3);
    for label in ["rollover_detected", "no_rollover", "unjudgeable"] {
        assert!(criteria[label].is_string());
    }
}

fn answer() -> Value {
    json!({"model":jev::MODEL,"answers":{"rollover":{"type":"choice","choice":"rollover_detected","confidence":0.95,"probabilities":{"rollover_detected":0.96,"no_rollover":0.02,"unjudgeable":0.02}}}})
}
#[test]
fn jev_validates_rollover_results_and_preserves_uncertainty() {
    let detected = jev::parse(&answer()).unwrap();
    assert_eq!(detected["status"], "rollover_detected");
    assert_eq!(detected["question_version"], "sensor-rollover-v2");
    let mut response = answer();
    response["answers"]["rollover"]["choice"] = json!("no_rollover");
    response["answers"]["rollover"]["probabilities"] =
        json!({"rollover_detected":0.02,"no_rollover":0.96,"unjudgeable":0.02});
    assert_eq!(jev::parse(&response).unwrap()["status"], "no_rollover");
    response["answers"]["rollover"]["confidence"] = json!(0.899);
    assert_eq!(jev::parse(&response).unwrap()["status"], "unjudgeable");
    assert_eq!(jev::parse(&response).unwrap()["decision_reason"], "low_model_confidence");
    let mut unexpected = answer();
    unexpected["answers"]["rollover"]["echoed_input"] = json!("private sensor information");
    assert!(
        jev::parse(&unexpected).unwrap()["answer"]
            .get("echoed_input")
            .is_none()
    );
    let mut response = answer();
    response["model"] = json!("other");
    assert!(jev::parse(&response).is_err());
    let mut response = answer();
    response["answers"]["rollover"]["probabilities"]["rollover_detected"] = json!(0.1);
    assert!(jev::parse(&response).is_err());
    let mut response = answer();
    response["answers"]["rollover"]["choice"] = json!("no_rollover");
    assert!(jev::parse(&response).is_err());
    response["answers"]["rollover"]["choice"] = json!("prone");
    assert!(jev::parse(&response).is_err());
}

#[test]
fn storage_isolates_beds_deduplicates_and_expires_after_six_hours() {
    let db = Database::new();
    let mut store = Store::open(&db.0, 21600, 100_000_000).unwrap();
    let m = sample(START);
    let raw = serde_json::to_string(&m).unwrap();
    store.insert(&m, &raw, START).unwrap();
    store.insert(&m, &raw, START + 1).unwrap();
    assert_eq!(
        store.window("bed-01", START + 1, START + 1).unwrap().len(),
        1
    );
    assert!(
        store
            .window("other-bed", START + 1, START + 1)
            .unwrap()
            .is_empty()
    );
    let mut conflicting = m.clone();
    conflicting.camera.face_area_px2 = Some(500.0);
    assert!(
        store
            .insert(
                &conflicting,
                &serde_json::to_string(&conflicting).unwrap(),
                START + 2
            )
            .is_err()
    );
    assert!(
        store
            .window("bed-01", START + 1, START + 21600)
            .unwrap()
            .is_empty()
    );
    store.expire(START + 21600).unwrap();
    drop(store);
    let mut reopened = Store::open(&db.0, 21600, 100_000_000).unwrap();
    assert!(
        reopened
            .window("bed-01", START + 1, START + 21600)
            .unwrap()
            .is_empty()
    );
}

#[test]
fn capacity_evicts_old_rows_and_continues_accepting_recent_data() {
    let db = Database::new();
    let mut store = Store::open(&db.0, 21600, 256 * 1024).unwrap();
    for second in START..START + 600 {
        let m = sample(second);
        store
            .insert(&m, &serde_json::to_string(&m).unwrap(), second)
            .unwrap();
        assert!(store.check_size().unwrap() <= 256 * 1024);
    }
    let recent = store.window("bed-01", START + 600, START + 600).unwrap();
    assert_eq!(recent.last().unwrap().measurement.timestamp, START + 599);
    assert!(recent.len() < 180);
}

fn app(db: &Database) -> Arc<App> {
    let token = "a".repeat(64);
    let hash = Sha256::digest(token.as_bytes())
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    Arc::new(App {
        config: Config {
            listen: "127.0.0.1:0".parse().unwrap(),
            database: db.0.clone(),
            tls_certificate: "unused".into(),
            tls_private_key: "unused".into(),
            registrations: None,
            retention_seconds: 21600,
            devices: vec![Device {
                bed_id: "bed-01".into(),
                token_sha256: hash,
                installation: json!({}),
            }],
        },
        store: Mutex::new(Store::open(&db.0, 21600, 100_000_000).unwrap()),
        registrations: None,
        jev_available: AtomicBool::new(false),
    })
}
fn post(token: &str, m: &Measurement) -> Request<Body> {
    Request::post("/v1/measurements")
        .header("Content-Type", "application/x-ndjson")
        .header("Authorization", format!("Bearer {token}"))
        .body(Body::from(format!(
            "{}\n",
            serde_json::to_string(m).unwrap()
        )))
        .unwrap()
}

#[tokio::test]
async fn receiver_enforces_authentication_bed_binding_and_committed_storage() {
    let db = Database::new();
    let app = app(&db);
    let service = router(app.clone());
    let m = sample(prone_guard_edge::now());
    assert_eq!(
        service
            .clone()
            .oneshot(post(&"b".repeat(64), &m))
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    let mut wrong = m.clone();
    wrong.bed_id = "other-bed".into();
    assert_eq!(
        service
            .clone()
            .oneshot(post(&"a".repeat(64), &wrong))
            .await
            .unwrap()
            .status(),
        StatusCode::FORBIDDEN
    );
    assert!(
        app.store
            .lock()
            .unwrap()
            .window("bed-01", m.timestamp + 1, m.timestamp)
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        service
            .clone()
            .oneshot(post(&"a".repeat(64), &m))
            .await
            .unwrap()
            .status(),
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        service
            .oneshot(post(&"a".repeat(64), &m))
            .await
            .unwrap()
            .status(),
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        app.store
            .lock()
            .unwrap()
            .window("bed-01", m.timestamp + 1, m.timestamp)
            .unwrap()
            .len(),
        1
    );
}

#[tokio::test]
async fn insufficient_data_skips_the_external_api_and_records_unjudgeable() {
    let db = Database::new();
    let app = app(&db);
    let client = reqwest::Client::builder().https_only(true).build().unwrap();
    prone_guard_edge::judge(
        app.clone(),
        client,
        Arc::new(Some("invalid-key-never-sent".into())),
        app.config.devices[0].clone(),
        prone_guard_edge::now(),
    )
    .await;
    assert!(!app.jev_available.load(Ordering::Relaxed));
    let connection = rusqlite::Connection::open(&db.0).unwrap();
    let result: String = connection
        .query_row("SELECT json FROM results", [], |row| row.get(0))
        .unwrap();
    let value: Value = serde_json::from_str(&result).unwrap();
    assert_eq!(value["status"], "unjudgeable");
    assert_eq!(value["question_version"], "sensor-rollover-v2");
    assert!(
        value["reason"]
            .as_str()
            .unwrap()
            .starts_with("insufficient_data")
    );
}

#[tokio::test]
async fn pairing_check_never_authorizes_measurements_until_commit_and_revocation_is_immediate() {
    use prone_guard_edge::{registration::Registration, registration_store::RegistrationStore};
    let db = Database::new();
    let registrations = Database::new();
    let mut app = app(&db);
    let id = "1".repeat(32);
    let edge = "2".repeat(32);
    let tx = "3".repeat(32);
    let token = "c".repeat(64);
    let mut registry = RegistrationStore::open(&registrations.0).unwrap();
    registry
        .prepare(Registration {
            device_id: id.clone(),
            edge_id: edge.clone(),
            transaction_id: tx.clone(),
            generation: 1,
            bed_id: "bed-01".into(),
            token_sha256: Sha256::digest(token.as_bytes())
                .iter()
                .map(|b| format!("{b:02x}"))
                .collect(),
            installation: json!({}),
        })
        .unwrap();
    let writable = Arc::get_mut(&mut app).unwrap();
    writable.config.devices.clear();
    writable.registrations = Some(Mutex::new(registry));
    let service = router(app.clone());
    let m = sample(prone_guard_edge::now());
    assert_eq!(
        service
            .clone()
            .oneshot(post(&token, &m))
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    let check = |transaction_id: &str| {
        Request::post("/v1/pairing/check").header("Content-Type","application/json").header("Authorization",format!("Bearer {token}")).body(Body::from(json!({"device_id":id,"edge_id":edge,"transaction_id":transaction_id,"generation":1}).to_string())).unwrap()
    };
    assert_eq!(
        service
            .clone()
            .oneshot(check(&"4".repeat(32)))
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    assert_eq!(
        service.clone().oneshot(check(&tx)).await.unwrap().status(),
        StatusCode::NO_CONTENT
    );
    assert_eq!(
        service
            .clone()
            .oneshot(post(&token, &m))
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    {
        let mut registry = app.registrations.as_ref().unwrap().lock().unwrap();
        assert_eq!(registry.status(&id).unwrap()["checked"], true);
        assert!(registry.activate(&id, &tx).is_err());
        registry.device_committed(&id, &tx, 1).unwrap();
        registry.activate(&id, &tx).unwrap();
    }
    assert_eq!(
        service
            .clone()
            .oneshot(post(&token, &m))
            .await
            .unwrap()
            .status(),
        StatusCode::NO_CONTENT
    );
    app.registrations
        .as_ref()
        .unwrap()
        .lock()
        .unwrap()
        .revoke(&id)
        .unwrap();
    let mut later = m;
    later.sequence += 1;
    assert_eq!(
        service
            .oneshot(post(&token, &later))
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
}

#[tokio::test]
async fn generated_edge_certificate_is_trusted_only_with_its_ca_and_hostname() {
    let db = Database::new();
    let identity =
        prone_guard_edge::identity::EdgeIdentity::generate(prone_guard_edge::now()).unwrap();
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let address = listener.local_addr().unwrap();
    let hostname = identity.hostname();
    let (certificate, key) = identity.tls_pem();
    let tls = axum_server::tls_rustls::RustlsConfig::from_pem(certificate, key)
        .await
        .unwrap();
    let server = tokio::spawn(
        axum_server::from_tcp_rustls(listener, tls)
            .unwrap()
            .serve(router(app(&db)).into_make_service()),
    );
    let client = reqwest::Client::builder()
        .https_only(true)
        .no_proxy()
        .resolve(&hostname, address)
        .resolve("wrong.local", address)
        .add_root_certificate(
            reqwest::Certificate::from_pem(identity.ca_certificate.as_bytes()).unwrap(),
        )
        .build()
        .unwrap();
    assert_eq!(
        client
            .get(format!("https://{hostname}:{}/health", address.port()))
            .send()
            .await
            .unwrap()
            .status(),
        200
    );
    assert!(
        client
            .get(format!("https://wrong.local:{}/health", address.port()))
            .send()
            .await
            .is_err()
    );
    let untrusted = reqwest::Client::builder()
        .https_only(true)
        .no_proxy()
        .resolve(&hostname, address)
        .build()
        .unwrap();
    assert!(
        untrusted
            .get(format!("https://{hostname}:{}/health", address.port()))
            .send()
            .await
            .is_err()
    );
    server.abort();
    let _ = server.await;
}
