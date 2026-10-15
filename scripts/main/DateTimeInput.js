.pragma library

// Parsing, validation and formatting of the date and time text typed (or
// picked) in TaskNest's date/time fields. Pure functions, no UI: the QML
// components and the tests both use this file.
//
// Canonical forms: dates "YYYY-MM-DD", times "HH:mm" (or "HH:mm:ss" when the
// seconds are not zero). Nothing here changes how timestamps are stored: the
// result is a local-time JavaScript Date that C++ converts to UTC as before.
//
// Accepted manual input
//   date  YYYY-MM-DD, also YYYY/MM/DD and YYYY.MM.DD (one separator kind,
//         used twice) and 1-digit month/day (2026-3-7)
//   time  H:mm, HH:mm, HH:mm:ss and the same with hyphens: HH-mm, HH-mm-ss
//         (24-hour; minutes and seconds are two digits; ":" and "-" are not mixed)
// Invalid values are reported, never rolled over into another day or month.

var MIN_YEAR = 1000;
var MAX_YEAR = 9999;

function pad(value, width) {
    var s = String(value);
    while (s.length < width)
        s = "0" + s;
    return s;
}

function isLeapYear(year) {
    return (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0;
}

// month is 1..12
function daysInMonth(year, month) {
    if (month === 2)
        return isLeapYear(year) ? 29 : 28;
    return (month === 4 || month === 6 || month === 9 || month === 11) ? 30 : 31;
}

var MONTH_NAMES = ["January", "February", "March", "April", "May", "June", "July",
                   "August", "September", "October", "November", "December"];

function monthName(month) {
    return MONTH_NAMES[month - 1];
}

function formatDate(year, month, day) {
    return pad(year, 4) + "-" + pad(month, 2) + "-" + pad(day, 2);
}

function formatTime(hour, minute, second) {
    var text = pad(hour, 2) + ":" + pad(minute, 2);
    if (second)
        text += ":" + pad(second, 2);
    return text;
}

function formatDateObject(date) {
    return formatDate(date.getFullYear(), date.getMonth() + 1, date.getDate());
}

function formatTimeObject(date) {
    return formatTime(date.getHours(), date.getMinutes(), date.getSeconds());
}

function capitalized(text) {
    return text.charAt(0).toUpperCase() + text.slice(1);
}

// -> { empty: true } | { ok: true, year, month, day, text } | { error }
function parseDate(input, label) {
    var name = label || "Date";
    var text = String(input).trim();
    if (text === "")
        return { empty: true };

    var m = /^(\d{4})([-\/.])(\d{1,2})\2(\d{1,2})$/.exec(text);
    if (!m)
        return { error: capitalized(name) + " date must look like 2026-12-31." };

    var year = Number(m[1]);
    var month = Number(m[3]);
    var day = Number(m[4]);
    var what = "That " + name.toLowerCase() + " date does not exist";
    if (year < MIN_YEAR || year > MAX_YEAR)
        return { error: what + " (years " + MIN_YEAR + "–" + MAX_YEAR + " are supported)." };
    if (month < 1 || month > 12)
        return { error: what + " (there is no month " + month + ")." };
    var last = daysInMonth(year, month);
    if (day < 1 || day > last) {
        return { error: what + " (" + monthName(month) + " " + year + " has "
                        + last + " days)." };
    }
    return { ok: true, year: year, month: month, day: day, text: formatDate(year, month, day) };
}

// -> { empty: true } | { ok: true, hour, minute, second, text } | { error }
function parseTime(input, label) {
    var name = capitalized(label || "Time");
    var text = String(input).trim();
    if (text === "")
        return { empty: true };

    var m = /^(\d{1,2})([:-])(\d{2})(?:\2(\d{2}))?$/.exec(text);
    if (!m) {
        return { error: name + " time must look like 14:30, 14:30:45 or 14-30 (24-hour)." };
    }
    var hour = Number(m[1]);
    var minute = Number(m[3]);
    var second = m[4] === undefined ? 0 : Number(m[4]);
    if (hour > 23 || minute > 59 || second > 59) {
        return { error: name + " time must be a valid 24-hour time "
                        + "(hour 0–23, minute 0–59, second 0–59)." };
    }
    return { ok: true, hour: hour, minute: minute, second: second,
             text: formatTime(hour, minute, second) };
}

// Combines a date and an optional time. Both blank means "none" (an invalid
// Date). A missing time becomes defaultHour:defaultMinute, so date-only values
// keep their historical meaning (due 23:59, reminder 09:00).
// -> { error } | { value: Date }
function parseDateTime(dateText, timeText, label, defaultHour, defaultMinute) {
    var date = parseDate(dateText, label);
    var time = parseTime(timeText, label);

    if (date.empty && time.empty)
        return { value: new Date(NaN) };
    if (date.error)
        return { error: date.error };
    if (date.empty)
        return { error: "Enter a " + label.toLowerCase() + " date, or clear the time." };
    if (time.error)
        return { error: time.error };

    var hour = time.empty ? defaultHour : time.hour;
    var minute = time.empty ? defaultMinute : time.minute;
    var second = time.empty ? 0 : time.second;

    var value = new Date(date.year, date.month - 1, date.day, hour, minute, second, 0);
    // A local time skipped by a clock change would silently become another
    // time: report it instead.
    if (value.getFullYear() !== date.year || value.getMonth() !== date.month - 1
            || value.getDate() !== date.day || value.getHours() !== hour
            || value.getMinutes() !== minute) {
        return { error: "That " + label.toLowerCase() + " time does not exist on "
                        + date.text + " (the clocks change)." };
    }
    return { value: value };
}

// Monday-first calendar cells for a month: 42 { year, month, day, inMonth }.
function monthCells(year, month) {
    var first = new Date(year, month - 1, 1);
    var lead = (first.getDay() + 6) % 7;
    var cells = [];
    for (var i = 0; i < 42; ++i) {
        var d = new Date(year, month - 1, 1 - lead + i, 12, 0, 0, 0);
        cells.push({ year: d.getFullYear(), month: d.getMonth() + 1, day: d.getDate(),
                     inMonth: d.getMonth() === month - 1 });
    }
    return cells;
}
