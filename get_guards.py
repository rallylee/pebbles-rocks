def run(filename):
    with open(filename) as fread:
        with open("guards.txt", "w") as fwrite:
            for line in fread:
                if line.startswith("--- level "):
                    fwrite.write(line)
                if line.startswith("Guard Key: "):
                    words = line.split()
                    if words[2] != '(bad)':
                        guard_key = words[2]
                        endidx = len(guard_key) - 17
                        fwrite.write(str(int(str(guard_key[2:endidx]), 16)))
                        fwrite.write("\n")

run("./tmp.txt")
