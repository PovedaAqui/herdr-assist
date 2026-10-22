"""Copia para src/shared os módulos que este firmware divide com o painel.

Tudo que é protocolo e estado (config na NVS, conexão com a ponte, descoberta,
pareamento, limites, log de atividade) não tem uma linha de LVGL nem de painel:
é exatamente a parte que não pode divergir entre os firmwares — um bug no
parse ou uma mudança no HMAC da descoberta precisa valer em todos. Em vez de
duplicar os arquivos no repositório (que divergem em silêncio) ou de apontar o
build_src_filter para fora do projeto (que o PlatformIO trata mal), o build
copia os originais de ../src a cada compilação.

Diferença em relação ao cardputer/sync_shared.py: lá a lista para no contrato
net.h, porque o Wi-Fi do Cardputer é a lib do Arduino. Aqui o firmware é
ESP-IDF como o painel, então net.c, pairing.c e fw_update.c vêm inteiros.
Ficam de fora, de propósito: i18n (o i18n.h inclui <lvgl.h>), sd.c (os pinos
do slot TF do painel, GPIO10-13, são os do e-paper aqui), term_parse e money
(o pager não mostra terminal nem custo).

Além da cópia, este script carimba HERDR_ASSIST_VERSION e define MANIFEST_URL
por env: o OTA do pager lê manifest-epaper154[-v1].json, nunca o manifest.json
de nome fixo, que é para sempre o do painel.

src/shared é gerado: está no .gitignore e qualquer edição ali se perde no
próximo build. Edite ../src.
"""

import os
import shutil
import subprocess

Import("env")  # noqa: F821  (injetado pelo PlatformIO/SCons)

PANEL_SRC = os.path.normpath(os.path.join(env.subst("$PROJECT_DIR"), "..", "src"))
DEST = os.path.join(env.subst("$PROJECT_SRC_DIR"), "shared")
FILES = (
    # protocolo e estado, iguais em todos os firmwares
    "discovery.c", "discovery.h",
    "panel_cfg.c", "panel_cfg.h",
    "herdr_model.c", "herdr_model.h",
    # dependência do herdr_model: a leitura de limites colapsa por conta
    "limits_merge.c", "limits_merge.h",
    # dependência do herdr_model: o diff de status alimenta o log de atividade
    "activity_log.c", "activity_log.h",
    "herdr_conn.c", "herdr_conn.h",
    "pairing.c", "pairing.h",
    # daqui para baixo é o que o Cardputer não pode compartilhar (não é Arduino)
    "net.c", "net.h",
    "fw_update.c", "fw_update.h",
)

os.makedirs(DEST, exist_ok=True)

missing = [f for f in FILES if not os.path.isfile(os.path.join(PANEL_SRC, f))]
if missing:
    raise SystemExit(
        "sync_shared: nao achei %s em %s — este projeto vive dentro do "
        "repositorio do herdr-assist e reaproveita o firmware do painel"
        % (", ".join(missing), PANEL_SRC)
    )

for name in FILES:
    src = os.path.join(PANEL_SRC, name)
    dst = os.path.join(DEST, name)
    # copia só quando muda: mtime igual evita recompilar o mundo a cada build
    if not os.path.isfile(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
        shutil.copy2(src, dst)
        print("sync_shared: %s atualizado" % name)

# módulo que saiu da lista não pode continuar compilando pela cópia velha
for name in os.listdir(DEST):
    if name not in FILES and os.path.isfile(os.path.join(DEST, name)):
        os.remove(os.path.join(DEST, name))
        print("sync_shared: %s removido (fora da lista)" % name)


def _version():
    """version.txt (CI) > git describe > 'dev'."""
    # SCons executa extra_scripts via exec(): __file__ não existe aqui
    proj = env.subst("$PROJECT_DIR")
    for cand in (
        os.path.join(proj, "..", "version.txt"),
        os.path.join(proj, "..", "..", "version.txt"),
    ):
        try:
            with open(cand) as fh:
                v = fh.read().strip()
            if v:
                return v
        except OSError:
            pass
    try:
        return subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=PANEL_SRC, capture_output=True, text=True,
            check=True).stdout.strip() or "dev"
    except (OSError, subprocess.CalledProcessError):
        return "dev"


# este é o único extra_script do projeto, então o carimbo de versão mora aqui
env.Append(CPPDEFINES=[("HERDR_ASSIST_VERSION", '\\"%s\\"' % _version())])

# OTA: cada revisão da placa tem o seu manifest (a imagem da V2 numa V1 sobe
# sem PSRAM; a da V1 numa V2 também) — e nenhum deles é o manifest.json
MANIFEST = "manifest-%s.json" % env.subst("$PIOENV")
env.Append(CPPDEFINES=[("MANIFEST_URL", '\\"%s\\"' % (
    "https://github.com/walcew/herdr-assist/releases/latest/download/" + MANIFEST))])
