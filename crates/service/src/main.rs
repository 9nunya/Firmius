#[tokio::main]
async fn main() {
    if let Err(error) = firmius_service::run_default().await {
        eprintln!("firmiusd: {error}");
        std::process::exit(1);
    }
}
