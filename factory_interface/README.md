# Factory Interface

A localhost-only web utility for helping a factory worker commission units.

## Prerequisites

- Python 3.12 or newer
- [uv](https://docs.astral.sh/uv/) installed and available on your `PATH`

## Start the Utility

From this directory:

```powershell
uv sync
uv run uvicorn factory_interface.app:app --host 127.0.0.1 --port 8000
```

Then open:

```text
http://127.0.0.1:8000
```

The app binds to `127.0.0.1` so it is only reachable from the local machine.

## Production Commissioning Session

Use this five-step runbook when commissioning production units. The examples keep the production
data repository next to the Leaf repository:

```text
<parent folder>/
  leaf/
  leaf_production_data/
```

### 1. Check out the latest production database

From the parent folder, clone the production data repository the first time:

```powershell
git clone https://github.com/DangerMonkeys/leaf_production_data.git
```

For later sessions, update the existing checkout and confirm it is clean:

```powershell
git -C leaf_production_data pull --ff-only origin main
git -C leaf_production_data status --short --branch
```

Copy the production database into `factory_interface` from the Leaf repository root:

```powershell
Copy-Item ..\leaf_production_data\leaf_devices.db factory_interface\leaf_devices.db -Force
```

### 2. Synchronize the factory interface environment

```powershell
Set-Location factory_interface
uv sync
```

### 3. Start the factory interface webserver

```powershell
uv run uvicorn factory_interface.app:app --host 127.0.0.1 --port 8000
```

Open `http://127.0.0.1:8000`. Keep the USB cable connected throughout commissioning; after
flashing, communication uses Wi-Fi, but USB remains the reliable power source during restarts.

### 4. Commission the units

Complete the setup checklist for the batch. Do not replace, copy, or commit `leaf_devices.db` while
the webserver is running.

### 5. Validate and push the updated production database

Stop the webserver with `Ctrl+C`, then validate the completed database:

```powershell
uv run python -c "import sqlite3; c=sqlite3.connect('leaf_devices.db'); print(c.execute('PRAGMA integrity_check').fetchone()[0])"
```

The result must be `ok`. Before copying the database back, fetch production data again and confirm
that `HEAD` still matches `origin/main`:

```powershell
git -C ..\..\leaf_production_data fetch origin main
git -C ..\..\leaf_production_data status --short --branch
git -C ..\..\leaf_production_data rev-parse HEAD
git -C ..\..\leaf_production_data rev-parse origin/main
```

If the two commit hashes differ, stop and reconcile the databases instead of overwriting another
commissioning session. If they match and the checkout is clean, copy, review, commit, and push:

```powershell
Copy-Item leaf_devices.db ..\..\leaf_production_data\leaf_devices.db -Force
git -C ..\..\leaf_production_data status --short
git -C ..\..\leaf_production_data add -- leaf_devices.db
git -C ..\..\leaf_production_data commit -m "Update commissioned Leaf devices database"
git -C ..\..\leaf_production_data push origin main
git -C ..\..\leaf_production_data status --short --branch
```

The final status should show a clean `main` branch synchronized with `origin/main`.

## Database Migrations

Alembic is configured for a local SQLite database at `leaf_devices.db`.

Create or update the database schema with:

```powershell
uv run alembic upgrade head
```

Create a new migration after changing SQLModel models with:

```powershell
uv run alembic revision --autogenerate -m "Describe change"
```
