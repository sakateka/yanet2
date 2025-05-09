use leptos::prelude::*;

use self::{
    admin::IconAdmin, aim::IconAim, arrow_down::IconArrowDown, cross::IconCross, error_bell::IconErrorBell,
    error_triangle::IconErrorTriangle, help::IconHelp, home::IconHome, logo::IconLogo, rack::IconRack,
    route::IconRoute, settings::IconSettings, tool::IconTool, triangle::IconTriangle,
};

mod admin;
mod aim;
mod arrow_down;
mod cross;
mod error_bell;
mod error_triangle;
mod help;
mod home;
mod logo;
mod rack;
mod route;
mod settings;
mod tool;
mod triangle;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IconKind {
    Logo,
    Home,
    Help,
    Cross,
    Settings,
    ErrorBell,
    ErrorTriangle,
    ArrowDown,
    Admin,
    Aim,
    Rack,
    Tool,
    Triangle,
    Route,
}

#[component]
pub fn Icon(
    // Icon type.
    #[prop(into)] kind: IconKind,
) -> impl IntoView {
    match kind {
        IconKind::Logo => view! { <IconLogo /> }.into_any(),
        IconKind::Home => view! { <IconHome /> }.into_any(),
        IconKind::Help => view! { <IconHelp /> }.into_any(),
        IconKind::Cross => view! { <IconCross /> }.into_any(),
        IconKind::Settings => view! { <IconSettings /> }.into_any(),
        IconKind::ErrorBell => view! { <IconErrorBell /> }.into_any(),
        IconKind::ErrorTriangle => view! { <IconErrorTriangle /> }.into_any(),
        IconKind::ArrowDown => view! { <IconArrowDown /> }.into_any(),
        IconKind::Admin => view! { <IconAdmin /> }.into_any(),
        IconKind::Aim => view! { <IconAim /> }.into_any(),
        IconKind::Rack => view! { <IconRack /> }.into_any(),
        IconKind::Tool => view! { <IconTool /> }.into_any(),
        IconKind::Triangle => view! { <IconTriangle /> }.into_any(),
        IconKind::Route => view! { <IconRoute /> }.into_any(),
    }
}
