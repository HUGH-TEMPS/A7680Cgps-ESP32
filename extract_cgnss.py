import pypdf

reader = pypdf.PdfReader('D:\\GPS\\A76XX_Series_AT_Command_Manual_V1.12.pdf')

with open('cgnssmode.txt', 'w', encoding='utf-8') as f:
    for i, page in enumerate(reader.pages):
        text = page.extract_text()
        if text and 'CGNSSMODE' in text:
            f.write(f'--- PAGE {i+1} ---\n')
            f.write(text)
            f.write('\n' + '='*80 + '\n')
