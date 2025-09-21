# Получаем ключи восстановления BitLocker для системного диска C:
$BitLockerRecoveryInfo = manage-bde -protectors -get C:

# Путь к файлу на рабочем столе текущего пользователя
$OutputPath = [Environment]::GetFolderPath("Desktop") + "\BitLockerRecoveryKey.txt"

# Сохраняем информацию в файл
$BitLockerRecoveryInfo | Out-File -FilePath $OutputPath -Encoding UTF8

Write-Output "Ключ восстановления BitLocker сохранён в $OutputPath"
