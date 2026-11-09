pub mod config;
pub mod discovery;
pub mod identity;
pub mod jev;
pub mod measurement;
pub mod pairing;
pub mod registration;
pub mod registration_store;
pub mod security2;
pub mod store;

use anyhow::Result;
use axum::{
    Json, Router,
    body::Bytes,
    extract::{DefaultBodyLimit, FromRequest, Request, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use serde_json::json;
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio::{
    task::JoinSet,
    time::{Instant, MissedTickBehavior},
};

pub struct App {
    pub config: config::Config,
    pub store: Mutex<store::Store>,
    pub registrations: Option<Mutex<registration_store::RegistrationStore>>,
    pub jev_available: AtomicBool,
}

pub fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock must be after 1970")
        .as_secs() as i64
}

pub fn router(app: Arc<App>) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/v1/measurements", post(receive))
        .route("/v1/pairing/check", post(pairing_check))
        .layer(DefaultBodyLimit::max(measurement::BODY_LIMIT))
        .with_state(app)
}

struct TimedBody<T>(T);

impl<S: Sync, T: FromRequest<S>> FromRequest<S> for TimedBody<T> {
    type Rejection = Response;

    async fn from_request(request: Request, state: &S) -> Result<Self, Self::Rejection> {
        tokio::time::timeout(Duration::from_secs(5), T::from_request(request, state))
            .await
            .map_err(|_| StatusCode::REQUEST_TIMEOUT.into_response())?
            .map(Self)
            .map_err(IntoResponse::into_response)
    }
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct PairingCheck {
    device_id: String,
    edge_id: String,
    transaction_id: String,
    generation: u32,
}

async fn pairing_check(
    State(app): State<Arc<App>>,
    headers: HeaderMap,
    TimedBody(Json(check)): TimedBody<Json<PairingCheck>>,
) -> impl IntoResponse {
    let Some(authorization) = headers
        .get("Authorization")
        .and_then(|v| v.to_str().ok())
        .map(str::to_owned)
    else {
        return StatusCode::UNAUTHORIZED;
    };
    let result = tokio::task::spawn_blocking(move || -> Result<bool> {
        let Some(registrations) = &app.registrations else {
            return Ok(false);
        };
        let mut registrations = registrations
            .lock()
            .map_err(|_| anyhow::anyhow!("registration lock poisoned"))?;
        if !registrations.pending_matches(
            &authorization,
            &check.device_id,
            &check.edge_id,
            &check.transaction_id,
            check.generation,
        )? {
            return Ok(false);
        }
        registrations.check(
            &authorization,
            &check.device_id,
            &check.edge_id,
            &check.transaction_id,
            check.generation,
        )?;
        Ok(true)
    })
    .await;
    match result {
        Ok(Ok(true)) => StatusCode::NO_CONTENT,
        Ok(Ok(false)) => StatusCode::UNAUTHORIZED,
        _ => StatusCode::SERVICE_UNAVAILABLE,
    }
}

async fn health(State(app): State<Arc<App>>) -> impl IntoResponse {
    let available = tokio::task::spawn_blocking({
        let app = app.clone();
        move || {
            app.store
                .lock()
                .is_ok_and(|store| store.writable && store.check_size().is_ok())
        }
    })
    .await
    .unwrap_or(false);
    (
        if available {
            StatusCode::OK
        } else {
            StatusCode::SERVICE_UNAVAILABLE
        },
        Json(
            json!({"receiver":if available {"ready"} else {"unavailable"},"jev":if app.jev_available.load(Ordering::Relaxed) {"available"} else {"not_confirmed"}}),
        ),
    )
}

async fn receive(
    State(app): State<Arc<App>>,
    headers: HeaderMap,
    TimedBody(body): TimedBody<Bytes>,
) -> impl IntoResponse {
    let Some(authorization) = headers
        .get("Authorization")
        .and_then(|v| v.to_str().ok())
        .map(str::to_owned)
    else {
        return StatusCode::UNAUTHORIZED;
    };
    if headers.get("Content-Type").and_then(|v| v.to_str().ok()) != Some("application/x-ndjson") {
        return StatusCode::UNSUPPORTED_MEDIA_TYPE;
    }
    let Ok(raw) = std::str::from_utf8(&body) else {
        return StatusCode::BAD_REQUEST;
    };
    let Some(raw) = raw.strip_suffix('\n') else {
        return StatusCode::BAD_REQUEST;
    };
    if raw.contains('\n') || raw.contains('\r') {
        return StatusCode::BAD_REQUEST;
    }
    let Ok(measurement) = serde_json::from_str::<measurement::Measurement>(raw) else {
        return StatusCode::BAD_REQUEST;
    };
    let received_at = now();
    if measurement.validate(received_at).is_err() {
        return StatusCode::BAD_REQUEST;
    }
    let bed = measurement.bed_id.clone();
    let raw = raw.to_owned();
    let result = tokio::task::spawn_blocking(move || -> Result<StatusCode> {
        // Hold registration access through the insert so revocation also covers in-flight requests.
        let registrations = app
            .registrations
            .as_ref()
            .map(|store| {
                store
                    .lock()
                    .map_err(|_| anyhow::anyhow!("registration lock poisoned"))
            })
            .transpose()?;
        let device = match &registrations {
            Some(store) => store.authenticate(&authorization)?,
            None => app.config.authenticate(&authorization).cloned(),
        };
        let Some(device) = device else {
            return Ok(StatusCode::UNAUTHORIZED);
        };
        if measurement.bed_id != device.bed_id {
            return Ok(StatusCode::FORBIDDEN);
        }
        app.store
            .lock()
            .map_err(|_| anyhow::anyhow!("store lock poisoned"))?
            .insert(&measurement, &raw, received_at)?;
        Ok(StatusCode::NO_CONTENT)
    })
    .await;
    match result {
        Ok(Ok(status)) => status,
        Ok(Err(error)) if error.to_string() == "sequence_conflict" => StatusCode::CONFLICT,
        _ => {
            eprintln!("{}", json!({"event":"storage_error","bed_id":bed}));
            StatusCode::SERVICE_UNAVAILABLE
        }
    }
}

pub async fn judge(
    app: Arc<App>,
    client: reqwest::Client,
    key: Arc<Option<String>>,
    device: config::Device,
    end: i64,
) {
    let bed = device.bed_id.clone();
    let rows = tokio::task::spawn_blocking({
        let app = app.clone();
        let bed = bed.clone();
        move || -> Result<_> {
            let mut store = app
                .store
                .lock()
                .map_err(|_| anyhow::anyhow!("store lock poisoned"))?;
            store.expire(now())?;
            store.window(&bed, end, now())
        }
    })
    .await;
    let mut result = match rows {
        Ok(Ok(rows)) => match measurement::prepare_window(rows, end) {
            Ok(_) if key.is_none() => json!({"status":"unjudgeable","reason":"jev_not_configured"}),
            Ok(window) => match jev::request(&window, end, &device.installation) {
                Ok(request) => match tokio::time::timeout(
                    Duration::from_secs(30),
                    jev::classify(&client, key.as_ref().as_ref().unwrap(), &request),
                )
                .await
                {
                    Ok(Ok(result)) if now() < end + measurement::PERIOD => {
                        app.jev_available.store(true, Ordering::Relaxed);
                        result
                    }
                    _ => {
                        app.jev_available.store(false, Ordering::Relaxed);
                        json!({"status":"unjudgeable","reason":"jev_failed_or_expired"})
                    }
                },
                Err(_) => json!({"status":"unjudgeable","reason":"input_too_large"}),
            },
            Err(error) => json!({"status":"unjudgeable","reason":error.to_string()}),
        },
        _ => json!({"status":"unjudgeable","reason":"storage_error"}),
    };
    result["requested_model"] = json!(jev::MODEL);
    result["question_version"] = json!(jev::QUESTION_VERSION);
    eprintln!(
        "{}",
        json!({"event":"posture_result","bed_id":bed,"window_start":end-180,"window_end_exclusive":end,"evaluated_at":now(),"result":result})
    );
    let saved = tokio::task::spawn_blocking(move || -> Result<()> {
        app.store
            .lock()
            .map_err(|_| anyhow::anyhow!("store lock poisoned"))?
            .result(&bed, end, now(), &result)
    })
    .await;
    if !matches!(saved, Ok(Ok(()))) {
        eprintln!(
            "{}",
            json!({"event":"result_storage_error","window_end_exclusive":end})
        );
    }
}

pub async fn scheduler(app: Arc<App>, key: Option<String>) -> Result<()> {
    let client = reqwest::Client::builder()
        .https_only(true)
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(30))
        .build()?;
    let key = Arc::new(key);
    let mut interval = tokio::time::interval_at(
        Instant::now() + Duration::from_secs(180),
        Duration::from_secs(180),
    );
    interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
    let mut previous_end = None;
    loop {
        interval.tick().await;
        let end = now();
        if previous_end.is_some_and(|previous| end - previous < 180) {
            eprintln!("{}", json!({"event":"clock_change","action":"skip_window"}));
            continue;
        }
        previous_end = Some(end);
        let mut jobs = JoinSet::new();
        let devices = match &app.registrations {
            Some(store) => store
                .lock()
                .map_err(|_| anyhow::anyhow!("registration lock poisoned"))?
                .devices()?,
            None => app.config.devices.clone(),
        };
        for device in devices {
            jobs.spawn(judge(app.clone(), client.clone(), key.clone(), device, end));
        }
        while let Some(result) = jobs.join_next().await {
            if result.is_err() {
                eprintln!("{}", json!({"event":"judgement_task_failed"}));
            }
        }
    }
}

pub async fn maintenance(app: Arc<App>) -> Result<()> {
    let mut interval = tokio::time::interval(Duration::from_secs(1));
    interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
    let mut failed = false;
    loop {
        interval.tick().await;
        let result = tokio::task::spawn_blocking({
            let app = app.clone();
            move || -> Result<()> {
                app.store
                    .lock()
                    .map_err(|_| anyhow::anyhow!("store lock poisoned"))?
                    .expire(now())
            }
        })
        .await;
        let unavailable = !matches!(result, Ok(Ok(())));
        if unavailable && !failed {
            eprintln!("{}", json!({"event":"retention_cleanup_failed"}));
        }
        failed = unavailable;
    }
}
