# Сторонние компоненты

* **7-Zip 26.03 / 7z.dll**, Igor Pavlov. Динамическая загрузка полной библиотеки. LGPL 2.1+ с ограничениями unRAR для RAR-декодеров и BSD-лицензиями отдельных компонентов. Полные тексты находятся в `licenses`. Исходный архив: `third-party-source/7zip-src.tar.xz` в portable-поставке и `third_party/7zip-src.tar.xz` в исходной. [Официальный проект](https://www.7-zip.org/).
* **libwebp 1.6.0**, WebM Project / Google. Статически связаны декодер и demux; тесты также используют encoder. BSD-лицензия и PATENTS включены. Исходный архив сопровождает поставку. [Официальный проект](https://chromium.googlesource.com/webm/libwebp).
* **Windows WIC, Direct2D, DirectWrite, winsqlite3** предоставляются Windows и не перепаковываются. Версия SQLite зависит от обновлений ОС, её двоичный файл не является закрепляемой сторонней поставкой Viewer.
* **RAR 6.24** использовался только для генерации тестовых архивов с собственными синтетическими изображениями. Программа RAR не включена в исходники или portable-папку; небольшие fixtures включены для воспроизводимых тестов.

Viewer не ограничивает замену совместимой 7z.dll пользователем или отладку такой замены. Закреплённые URL и SHA-256 исходных зависимостей: `third_party/dependencies.json`.

* **libjpeg-turbo 3.2.0**, динамический официальный VC x64 runtime. IJG/BSD/zlib: тексты LICENSE.md и README.ijg включены в licenses, исходный архив — в third-party-source. URL и SHA-256 исходников и runtime закреплены в dependencies.json. https://libjpeg-turbo.org/

* **zlib 1.3.2**, Jean-loup Gailly / Mark Adler. Статическая библиотека для ZIP-сжатия PSD. Лицензия в licenses/zlib.txt, исходники в third-party-source/zlib.tar.gz. https://zlib.net/
