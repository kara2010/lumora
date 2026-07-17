<?php
// Einzelstream-Endpunkt: der geteilte Zuschauer-Link heisst bewusst stream.php
// (ein Einzelstream ist keine "Gruppe"). Die komplette Logik (Shim ?s=,
// Domain-Player ?p=, Grid ?code=, API a=...) lebt unveraendert in gruppe.php
// und wird hier nur inkludiert. Alle vom Shim und Player erzeugten Ziele sind
// RELATIV ("?p=...", "?code=...") und bleiben damit automatisch auf stream.php.
// Alte gruppe.php?s=-Links (Clients bis 2.2.18) funktionieren unveraendert.
require __DIR__ . '/gruppe.php';
