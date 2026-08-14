# Выпуск IBStart

1. Обновите номер версии в `CMakeLists.txt`, `CHANGELOG.md` и ресурсах, если это необходимо.
2. Откройте x64 Native Tools Command Prompt for VS 2022 и выполните:

   ```powershell
   cmake --preset vs2022-x64-release
   cmake --build --preset vs2022-x64-release
   ctest --preset vs2022-x64-release
   ```

3. Убедитесь, что post-build проверка прошла: `IBStart.exe` не больше 8 МиБ. Проверьте запуск на чистой Windows 10/11 с 32- и 64-разрядной 1С.
4. Сформируйте архив, содержащий `IBStart.exe`, `README.md`, `LICENSE`, `CHANGELOG.md`; создайте SHA-256 `checksums.txt`.
5. Создайте аннотированный тег `vX.Y.Z`. Workflow `release.yml` повторно собирает, тестирует, создаёт архив и публикует GitHub Release с EXE, ZIP и checksums.

Нельзя добавлять упаковщики EXE, автообновление, телеметрию или секреты в артефакты. Перед тегированием вручную проверьте, что в логах нет пароля, токена либо строк `/P` с исходным значением.
