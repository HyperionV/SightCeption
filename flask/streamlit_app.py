import time
from urllib.parse import urljoin

import requests
import streamlit as st


st.set_page_config(page_title="SightCeption Dashboard", page_icon="👓", layout="wide")
st.title("SightCeption Dashboard")


with st.sidebar:
    st.header("Settings")
    st.session_state["backend_url"] = st.text_input(
        "Backend URL",
        value=st.session_state.get("backend_url", "http://127.0.0.1:5000/"),
        help="Flask server base URL",
        key="backend_url_input",
    )
    st.caption("Backend must be running: Flask app.py")


def get_backend_base_url() -> str:
    base = st.session_state.get("backend_url", "http://127.0.0.1:5000/")
    if not base.endswith('/'):
        base += '/'
    return base


def api_get(path: str, timeout: float = 8.0):
    base = get_backend_base_url()
    try:
        r = requests.get(urljoin(base, path.lstrip('/')), timeout=timeout)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        st.error(f"GET {path}: {e}")
        return None


def api_post(path: str, timeout: float = 8.0):
    base = get_backend_base_url()
    try:
        r = requests.post(urljoin(base, path.lstrip('/')), timeout=timeout)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        st.error(f"POST {path}: {e}")
        return None


def full_image_url(relative_or_abs: str | None) -> str | None:
    if not relative_or_abs:
        return None
    if relative_or_abs.startswith('http://') or relative_or_abs.startswith('https://'):
        return f"{relative_or_abs}?t={int(time.time())}"
    return urljoin(get_backend_base_url(), f"{relative_or_abs.lstrip('/')}?t={int(time.time())}")


tab1, tab2, tab3 = st.tabs(["Adjust Camera Angle", "Test Object Detection", "Activity Log"])


with tab1:
    st.subheader("Adjust Camera Angle")
    col_a, col_b = st.columns([1, 1])

    with col_a:
        if st.button("Capture Image", type="primary"):
            res = api_post("/api/capture")
            if res:
                st.session_state.preview_url = full_image_url(res.get("latest_image_url"))
                if not st.session_state.preview_url:
                    # Fallback: status
                    st.session_state.preview_url = full_image_url(api_get("/api/status").get("latest_image_url"))
        if st.button("Refresh Preview"):
            st.session_state.preview_url = full_image_url(api_get("/api/status").get("latest_image_url"))

    with col_b:
        prev = st.session_state.get("preview_url")
        if prev:
            st.image(prev, caption="Latest image", use_column_width=True)
        else:
            st.info("No image yet. Click 'Capture Image'.")


with tab2:
    st.subheader("Test Object Detection")
    if st.button("Run Detection", type="primary"):
        res = api_post("/api/detect")
        if res:
            st.session_state.detect_img = full_image_url(res.get("latest_image_url"))
            st.session_state.detected = res.get("detected", [])

    detected = st.session_state.get("detected", [])
    img = st.session_state.get("detect_img")

    cols = st.columns([2, 1])
    with cols[0]:
        if img:
            st.image(img, caption="Detection image", use_column_width=True)
        else:
            st.info("Run detection to see the latest frame.")
    with cols[1]:
        st.markdown("**Detected objects**")
        if detected:
            for cls in detected:
                st.success(cls)
        else:
            st.write("None")


with tab3:
    st.subheader("Activity Log")
    colr1, colr2 = st.columns([1, 3])
    with colr1:
        auto = st.checkbox("Auto refresh", value=True)
        refresh_ms = st.slider("Interval (ms)", 500, 5000, 1500, 500)

    data = api_get("/api/status") or {}
    activity = data.get("activity", [])
    if activity:
        # Show newest first
        activity = list(reversed(activity))
        st.dataframe(activity, use_container_width=True, hide_index=True)
    else:
        st.info("No activity yet.")

    # Lightweight JS-driven auto-refresh (no extra deps)
    if auto:
        st.markdown(
            f"""
            <script>
                setTimeout(function() {{ window.location.reload(); }}, {int(refresh_ms)});
            </script>
            """,
            unsafe_allow_html=True,
        )


