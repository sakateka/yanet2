use leptos::prelude::*;
use leptos_router::{
    components::{Route, Router, Routes},
    path,
};
use settings::SideBarContext;

mod api;
mod components;
// mod i18n;
mod settings;
mod theme;
mod xxx;

use crate::{
    components::{
        common::snackbar::Snackbar, demo::DemoView, home::Home, nav::Nav, neighbour::NeighbourView,
        route::lookup::RouteLookupView, settings::SettingsView,
    },
    theme::ThemeContext,
};

#[component]
pub fn App() -> impl IntoView {
    provide_context(ThemeContext::new());
    provide_context(SideBarContext::new());

    view! {
        <Snackbar>
            <div class="app">
                <Router>
                    <Nav />
                    <main>
                        <Routes fallback=|| "Not found.">
                            <Route path=path!("/") view=Home />
                            <Route path=path!("/neighbour") view=NeighbourView />
                            <Route path=path!("/route/lookup") view=RouteLookupView />
                            <Route path=path!("/demo") view=DemoView />
                            <Route path=path!("/settings") view=|| view! { <SettingsView /> } />
                        </Routes>
                    </main>
                </Router>
            </div>
        </Snackbar>
    }
}
