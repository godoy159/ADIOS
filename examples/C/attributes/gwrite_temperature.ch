adios_groupsize = 4 \
                + 4 \
                + 4 \
                + 8 \
                + strlen(str) \
                + 8 * (1) * (NX);
adios_group_size (adios_handle, adios_groupsize, &adios_totalsize, &comm);
adios_write (adios_handle, "NX", &NX);
adios_write (adios_handle, "size", &size);
adios_write (adios_handle, "rank", &rank);
adios_write (adios_handle, "mean", &mean);
adios_write (adios_handle, "date", str);
adios_write (adios_handle, "temperature", t);
