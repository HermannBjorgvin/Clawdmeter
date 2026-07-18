// calnext — печатает JSON-список встреч из системного календаря macOS на
// ближайшие 26 часов: [{"start": <epoch>, "title": "..."}, ...]. Хелпер для
// claude_usage_daemon.py (cal_ics_url = eventkit): у голого python-бинаря
// нет Info.plist с NSCalendars*UsageDescription, поэтому macOS молча
// отказывает ему в EventKit; у этого бинаря описание встроено линкером
// (-sectcreate __TEXT __info_plist), так что системный диалог разрешения
// появляется как положено. Сборка: ./build_calnext.sh
import EventKit
import Foundation

let store = EKEventStore()
let sem = DispatchSemaphore(value: 0)
var granted = false
if #available(macOS 14.0, *) {
    store.requestFullAccessToEvents { ok, _ in granted = ok; sem.signal() }
} else {
    store.requestAccess(to: .event) { ok, _ in granted = ok; sem.signal() }
}
// Первый запуск ждёт, пока пользователь ответит на системный диалог.
_ = sem.wait(timeout: .now() + 120)
guard granted else {
    FileHandle.standardError.write(Data("calendar access denied\n".utf8))
    exit(2)
}

let now = Date()
let end = now.addingTimeInterval(26 * 3600)   // = CAL_HORIZON_S демона
let pred = store.predicateForEvents(withStart: now, end: end, calendars: nil)
var out: [[String: Any]] = []
for ev in store.events(matching: pred) {
    if ev.isAllDay || ev.status == .canceled { continue }
    guard let start = ev.startDate else { continue }
    out.append(["start": start.timeIntervalSince1970,
                "title": ev.title ?? ""])
}
FileHandle.standardOutput.write(try JSONSerialization.data(withJSONObject: out))
