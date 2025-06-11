## Para buildar

Use o arquivo `build.sh` nos scripts para baixar e compilar a versão do kernel Linux vulnerável. O script também baixa e compila o BusyBox.

## O ambiente

Todo o ambiente do linux também é feito no arquivo `build.sh`. Colocamos as permissões e configurações básicas lá. Inclusive, o arquivo /dev/shmem/target.txt que usamos como target do exploit é configurado lá.

## Para virtualizar

Para virtualizar o kernel vulnerável, os scripts `launch*.sh` usam o qemu para diferentes versões do kernel, que no meu computador original eu salvei em pastas com nomes diferentes. Os arquivos de interesse (como o arquivo de exploit, benchmark e perf) devem ficar na pasta initramfs do busybox. Para fazer alguma alteração e colocar outro binário na pasta, ele precisa estar compilado estaticamente, já que o BusyBox não vem com a maioria das bibliotecas necessárias para rodar um arquivo ELF comum linkado dinamicamente. 

## Os patches

As alterações que eu fiz no source code do Linux se encontram nas pastas `patch_logico` e `mutex`. Coloquei apenas os arquivos isolados que eu alterei lá.

## Uso do perf

Usei o seguinte comando para ver o desempenho do exploit:

`./perf stat -r 5 -e cycles,instructions,context-switches,cpu-clock,page-faults ./reproducer /dev/shmem/target.txt`

O perf foi compilado estaticamente e esse pequeno blogpost ajudou bastante: https://wangzhou.github.io/how-to-compile-perf-tool-statically
