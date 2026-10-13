pragma Singleton
import QtQuick

// TaskNest design system: "quietly futuristic productivity".
// Graphite surfaces with a cool undertone, one electric-lime accent used only
// for actions and the active state, a violet secondary for reminders, and
// warm amber / red reserved for time pressure. Nothing outside this file
// should hard-code a color, size, radius or duration.
QtObject {
    id: theme

    // ---- motion ---------------------------------------------------------
    // Set from main.cpp when Windows "animation effects" are turned off.
    property bool reducedMotion: false
    readonly property int durationFast: reducedMotion ? 0 : 110
    readonly property int durationBase: reducedMotion ? 0 : 170

    // ---- surfaces -------------------------------------------------------
    readonly property color bg: "#0E1014"            // window
    readonly property color sidebar: "#12151B"       // navigation rail
    readonly property color surface: "#171A21"       // main workspace
    readonly property color surfaceHover: "#1C2028"  // row hover
    readonly property color surfaceRaised: "#202530" // inputs, chips
    readonly property color elevated: "#262B37"      // dialogs, popups
    readonly property color scrim: "#B8080A0E"       // behind modal dialogs
    readonly property color border: "#2B313C"
    readonly property color borderStrong: "#3D4555"

    // ---- text -----------------------------------------------------------
    readonly property color text: "#E8EAF0"          // primary (not pure white)
    readonly property color textMuted: "#A2AABA"     // secondary, >= 6:1 on surface
    readonly property color textFaint: "#7D8697"     // hints, >= 4.5:1 on surface

    // ---- accent: electric lime -----------------------------------------
    readonly property color accent: "#B8F23C"
    readonly property color accentHover: "#C8FA58"
    readonly property color accentPressed: "#9ED42B"
    readonly property color accentText: "#0F1405"      // text/icons on lime
    readonly property color accentSoft: Qt.rgba(0.722, 0.949, 0.235, 0.13)
    readonly property color accentLine: Qt.rgba(0.722, 0.949, 0.235, 0.45)
    readonly property color focus: accent

    // ---- secondary: violet (reminders) ---------------------------------
    readonly property color secondary: "#A396FF"
    readonly property color secondarySoft: Qt.rgba(0.64, 0.59, 1.0, 0.14)
    readonly property color secondaryLine: Qt.rgba(0.64, 0.59, 1.0, 0.40)

    // ---- status ---------------------------------------------------------
    readonly property color success: "#3DDC97"
    readonly property color successSoft: Qt.rgba(0.24, 0.86, 0.59, 0.14)
    readonly property color successLine: Qt.rgba(0.24, 0.86, 0.59, 0.40)
    readonly property color warning: "#F5B544"
    readonly property color warningSoft: Qt.rgba(0.96, 0.71, 0.27, 0.14)
    readonly property color danger: "#FF7070"
    readonly property color dangerSoft: Qt.rgba(1.0, 0.44, 0.44, 0.13)
    readonly property color dangerLine: Qt.rgba(1.0, 0.44, 0.44, 0.45)

    // ---- typography -----------------------------------------------------
    // Single family names: if one is missing, Qt falls back to the system font.
    readonly property string fontFamily: "Segoe UI Variable Text"
    readonly property string displayFamily: "Bahnschrift"
    // px sizes: display / heading / title / body / label / caption / overline
    readonly property int fontDisplay: 28
    readonly property int fontHeading: 18
    readonly property int fontTitle: 15
    readonly property int fontBody: 14
    readonly property int fontLabel: 13
    readonly property int fontCaption: 12
    readonly property int fontOverline: 11

    // ---- spacing scale --------------------------------------------------
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 24
    readonly property int s6: 32

    // ---- shape ----------------------------------------------------------
    readonly property int radiusSm: 6     // rows, buttons, chips
    readonly property int radiusMd: 10    // inputs, banners
    readonly property int radiusLg: 16    // dialogs
    readonly property int controlHeight: 38
    readonly property int rowHeight: 54
    readonly property int sidebarWidth: 240
    readonly property int railWidth: 68
    readonly property int railBreakpoint: 880 // window narrower than this: icon rail

    // ---- icons: 24x24 grid, stroked ------------------------------------
    readonly property var icons: ({
        "all": "M9 6h11 M9 12h11 M9 18h11 M4.5 6h.01 M4.5 12h.01 M4.5 18h.01",
        "today": "M5 5h14a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z M4 10h16 M8 3v4 M16 3v4 M12 15h.01",
        "upcoming": "M4 12h15 M13 6l6 6-6 6",
        "completed": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z M8 12.5l2.7 2.7L16 9.5",
        "inbox": "M4 14l2-8h12l2 8 M4 14v4a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-4 M4 14h4.5l1 2h5l1-2H20",
        "list": "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z",
        "plus": "M12 5v14 M5 12h14",
        "search": "M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14z M20 20l-4-4",
        "bell": "M6 16v-5a6 6 0 1 1 12 0v5l2 2H4z M10 21h4",
        "edit": "M4 20h4L19 9l-4-4L4 16z M13.5 6.5l4 4",
        "trash": "M5 7h14 M10 7V4h4v3 M7 7l1 13h8l1-13 M10 11v5 M14 11v5",
        "close": "M6 6l12 12 M18 6L6 18",
        "check": "M5 12.5l4.5 4.5L19 7.5",
        "warning": "M12 4l9 16H3z M12 10v4 M12 17h.01",
        "clock": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z M12 7v5l3 2",
        "chevron": "M6 9l6 6 6-6",
        "panel": "M4 5h16v14H4z M9 5v14",
        "undo": "M9 14L4 9l5-5 M4 9h10a6 6 0 0 1 0 12h-3",
        "keyboard": "M4 6h16a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1z M7 10h.01 M11 10h.01 M15 10h.01 M7 14h10",
        "flag": "M6 21V4 M6 5h11l-2.5 4L17 13H6",
        "nest": "M4 11a8 8 0 0 0 16 0 M8.5 8l2.5 2.5L15.5 5"
    })

    // ---- date helpers ---------------------------------------------------
    function _dayLabel(date, dayDiff) {
        if (dayDiff === 0) return "Today"
        if (dayDiff === 1) return "Tomorrow"
        if (dayDiff === -1) return "Yesterday"
        if (dayDiff > 1 && dayDiff < 7) return Qt.formatDate(date, "dddd")
        const sameYear = date.getFullYear() === new Date().getFullYear()
        return Qt.formatDate(date, sameYear ? "d MMM" : "d MMM yyyy")
    }

    // { text, state } for a due date, or null when there is none.
    // state: "overdue" | "today" | "soon" | "later" | "done"
    // Date-only due dates are stored as 23:59 and shown without a time.
    function dueInfo(date, completed) {
        if (date === undefined || date === null || isNaN(date.getTime()))
            return null
        const now = new Date()
        const startToday = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        const startDue = new Date(date.getFullYear(), date.getMonth(), date.getDate())
        const dayDiff = Math.round((startDue.getTime() - startToday.getTime()) / 86400000)
        const dateOnly = date.getHours() === 23 && date.getMinutes() === 59
        const time = dateOnly ? "" : ", " + Qt.formatTime(date, "HH:mm")
        const label = _dayLabel(date, dayDiff)

        if (completed)
            return { text: label + time, state: "done" }
        if (date.getTime() < now.getTime())
            return { text: "Overdue · " + label + time, state: "overdue" }
        if (dayDiff === 0)
            return { text: label + time, state: "today" }
        if (date.getTime() - now.getTime() < 86400000)
            return { text: label + time, state: "soon" }
        return { text: label + time, state: "later" }
    }

    function dueColor(state) {
        switch (state) {
        case "overdue": return danger
        case "today":
        case "soon": return warning
        case "done": return textFaint
        default: return textMuted
        }
    }

    function dueIcon(state) {
        if (state === "overdue") return "warning"
        if (state === "today" || state === "soon") return "clock"
        return "today"
    }

    // Parses a local date + optional time typed as YYYY-MM-DD and HH:MM.
    // Both blank means "none". Returns { error: string } or { value: Date }
    // (an invalid Date means "none"). `label` names the field in messages; a
    // missing time becomes hours:minutes.
    function parseDateTime(dateText, timeText, label, hours, minutes) {
        dateText = dateText.trim()
        timeText = timeText.trim()
        if (dateText === "" && timeText === "")
            return { value: new Date(NaN) }
        if (dateText === "")
            return { error: "Enter a " + label.toLowerCase() + " date, or clear the time." }

        const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateText)
        if (!d)
            return { error: label + " date must look like 2026-12-31." }
        if (timeText !== "") {
            const t = /^(\d{1,2}):(\d{2})$/.exec(timeText)
            if (!t || Number(t[1]) > 23 || Number(t[2]) > 59)
                return { error: label + " time must look like 14:30 (24-hour)." }
            hours = Number(t[1])
            minutes = Number(t[2])
        }
        const date = new Date(Number(d[1]), Number(d[2]) - 1, Number(d[3]), hours, minutes, 0, 0)
        if (date.getFullYear() !== Number(d[1]) || date.getMonth() !== Number(d[2]) - 1
                || date.getDate() !== Number(d[3]))
            return { error: "That " + label.toLowerCase() + " date does not exist." }
        return { value: date }
    }

    function reminderText(date) {
        return Qt.formatDateTime(date, "d MMM, HH:mm")
    }
}
