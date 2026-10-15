import QtQuick
import QtTest
import "../../scripts/main/DateTimeInput.js" as DT

// Pure parsing/validation tests for the date and time input rules.
TestCase {
    name: "DateTimeInput"

    function test_dateAcceptsCanonicalAndReasonableVariants() {
        compare(DT.parseDate("2026-12-31").text, "2026-12-31")
        compare(DT.parseDate("  2026-12-31 ").text, "2026-12-31")
        compare(DT.parseDate("2026/03/07").text, "2026-03-07")
        compare(DT.parseDate("2026.3.7").text, "2026-03-07")
        compare(DT.parseDate("2026-3-7").text, "2026-03-07")
        verify(DT.parseDate("").empty)
        verify(DT.parseDate("   ").empty)
    }

    function test_dateRejectsInvalidAndAmbiguousInput() {
        const bad = ["31/12/2026", "31-12-2026", "12-31-2026", "26-12-31", "2026-12", "2026",
                     "2026-12-31 10:00", "2026-12-31T10:00", "2026-12/31", "2026--12-31",
                     "yesterday", "2026-1a-01", "２０２６-12-31"]
        for (let i = 0; i < bad.length; ++i) {
            const r = DT.parseDate(bad[i], "Due")
            verify(r.error !== undefined, "should be refused: " + bad[i])
            compare(r.ok, undefined)
        }
        verify(DT.parseDate("31/12/2026", "Due").error.indexOf("Due date must look like") === 0)
    }

    function test_leapYearsAndMonthBoundaries() {
        verify(DT.parseDate("2024-02-29").ok)
        verify(DT.parseDate("2000-02-29").ok)
        verify(DT.parseDate("2023-02-29").error.indexOf("does not exist") >= 0)
        verify(DT.parseDate("2100-02-29").error.indexOf("does not exist") >= 0)
        verify(DT.parseDate("2026-04-30").ok)
        verify(DT.parseDate("2026-04-31").error.indexOf("April 2026 has 30 days") >= 0)
        verify(DT.parseDate("2026-12-31").ok)
        verify(DT.parseDate("2026-13-01").error.indexOf("no month 13") >= 0)
        verify(DT.parseDate("2026-00-10").error.indexOf("no month 0") >= 0)
        verify(DT.parseDate("2026-05-00").error.indexOf("does not exist") >= 0)
        verify(DT.parseDate("0999-01-01").error.indexOf("does not exist") >= 0)
        compare(DT.daysInMonth(2024, 2), 29)
        compare(DT.daysInMonth(2026, 2), 28)
        compare(DT.daysInMonth(2026, 11), 30)
        compare(DT.daysInMonth(2026, 12), 31)
    }

    function test_timeFormatsAndHyphenNormalization() {
        compare(DT.parseTime("14:30").text, "14:30")
        compare(DT.parseTime("9:05").text, "09:05")
        compare(DT.parseTime("14:30:45").text, "14:30:45")
        compare(DT.parseTime("14:30:00").text, "14:30")   // seconds only when they matter
        compare(DT.parseTime("14-30").text, "14:30")
        compare(DT.parseTime("9-05").text, "09:05")
        compare(DT.parseTime("14-30-45").text, "14:30:45")
        compare(DT.parseTime(" 07:00 ").text, "07:00")
        verify(DT.parseTime("").empty)
    }

    function test_timeBoundaries() {
        compare(DT.parseTime("00:00").text, "00:00")      // midnight
        compare(DT.parseTime("12:00").text, "12:00")      // noon
        compare(DT.parseTime("23:59:59").text, "23:59:59")
        compare(DT.parseTime("00:00:01").text, "00:00:01")
        const r = DT.parseTime("00:00")
        compare(r.hour, 0)
        compare(r.minute, 0)
    }

    function test_timeRejectsInvalidInput() {
        const bad = ["24:00", "12:60", "12:30:60", "99:99", "7pm", "7", "1230", "12:5", "12:3:45",
                     "12:30-15", "12-30:15", "12:30:45:10", "12 30", "12.30", "-12:30", "ab:cd",
                     "2026-12-31", "12:30pm"]
        for (let i = 0; i < bad.length; ++i) {
            const r = DT.parseTime(bad[i], "Reminder")
            verify(r.error !== undefined, "should be refused: " + bad[i])
            verify(r.error.indexOf("Reminder time must") === 0, r.error)
        }
    }

    function test_dateTimeCombinesWithoutRolling() {
        let r = DT.parseDateTime("2026-12-31", "23-59-30", "Due", 23, 59)
        compare(r.value.getFullYear(), 2026)
        compare(r.value.getMonth(), 11)
        compare(r.value.getDate(), 31)
        compare(r.value.getSeconds(), 30)

        // the default time applies only when the time is left blank
        r = DT.parseDateTime("2026-06-01", "", "Reminder", 9, 0)
        compare(r.value.getHours(), 9)
        compare(r.value.getMinutes(), 0)
        r = DT.parseDateTime("2026-06-01", "00:00", "Reminder", 9, 0)
        compare(r.value.getHours(), 0)
        r = DT.parseDateTime("2026-06-01", "12:00", "Reminder", 9, 0)
        compare(r.value.getHours(), 12)

        // leap day
        r = DT.parseDateTime("2024-02-29", "12:00", "Due", 23, 59)
        compare(r.value.getDate(), 29)
        compare(r.value.getMonth(), 1)
        r = DT.parseDateTime("2025-02-29", "12:00", "Due", 23, 59)
        verify(r.error.indexOf("does not exist") >= 0)
        compare(r.value, undefined)

        // nothing entered = none; a time without a date is an error
        verify(isNaN(DT.parseDateTime("", "", "Due", 23, 59).value.getTime()))
        verify(DT.parseDateTime("", "10:00", "Due", 23, 59).error.indexOf("due date") >= 0)
        verify(DT.parseDateTime("2026-06-01", "25:00", "Due", 23, 59).error.indexOf("time must") >= 0)
    }

    function test_monthCellsAreMondayFirstAndCoverTheMonth() {
        // October 2026 starts on a Thursday.
        const cells = DT.monthCells(2026, 10)
        compare(cells.length, 42)
        compare(cells[0].day, 28)       // Mon 28 Sep
        compare(cells[0].inMonth, false)
        compare(cells[3].day, 1)
        compare(cells[3].inMonth, true)
        let inMonth = 0
        for (let i = 0; i < cells.length; ++i)
            inMonth += cells[i].inMonth ? 1 : 0
        compare(inMonth, 31)
        // February 2024 (leap) has 29 days on the grid
        const feb = DT.monthCells(2024, 2)
        let febDays = 0
        for (let i = 0; i < feb.length; ++i)
            febDays += feb[i].inMonth ? 1 : 0
        compare(febDays, 29)
    }
}
