use leptos::prelude::*;

#[component]
pub fn IconTriangle() -> impl IntoView {
    view! {
        <svg
            xmlns="http://www.w3.org/2000/svg"
            xmlns:xlink="http://www.w3.org/1999/xlink"
            width="16"
            height="10"
            fill="currentColor"
            stroke="none"
            aria-hidden="true"
        >
            <svg viewBox="0 0 8 8" fill="currentColor" xmlns="http://www.w3.org/2000/svg">
                <path d="m.72 7.64 6.39-3.2a.5.5 0 0 0 0-.89L.72.36A.5.5 0 0 0 0 .81v6.38c0 .37.4.61.72.45Z"></path>
            </svg>
        </svg>
    }
}
