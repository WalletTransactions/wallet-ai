# Weave Pay — обезличенный образец

`statement.txt` — текст выписки по карте Weave Pay (PDF, 2026-10-26) после
`pdftotext -layout`, как его видит сервис. Владелец, адрес, IBAN, WeavePay ID и
номер карты заменены, суммы умножены на 0.73; продавцы, даты, статусы и
раскладка строк (с водяным знаком «This is not an official statement»,
рассыпанным по строкам отдельными буквами) сохранены.

18 строк таблицы: 16 операций (14 Approved, 2 Pending) и 2 отклонённых
(Declined). Профиль — `prompts/banks/weavepay.txt`, тест —
`weavepay_sample_detect_and_dates` в `tests/test_statement.cpp`.
