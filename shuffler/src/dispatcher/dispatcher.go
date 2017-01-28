// Copyright 2016 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Shuffler receives ciphertexts from Encoders (end users), buffers them
// according to a policy, and then batch sends them in a random order to an
// Analyzer. The purpose is to break linkability between end users and
// ciphertexts from the Analyzer's point of view. The Analyzer does not know
// which end user produced which ciphertext.

package dispatcher

import (
	"fmt"
	"time"

	"github.com/golang/glog"
	"golang.org/x/net/context"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"

	shufflerpb "cobalt"
	"storage"
)

// dispatchDelayInS adds a tiny delay between grpc calls to Analyzer.
const dispatchDelayInS = 2

// minWaitTimeInS adds a small delay to throttle dispatch attempt checks that
// happen very frequenctly.
const minWaitTimeInS = 3

// AnalyzerTransport is an interface for Analyzer where the observations get
// collected, analyzed and reported.
type AnalyzerTransport interface {
	send(obBatch *shufflerpb.ObservationBatch) error
	close()
	reconnect()
}

// GrpcClientConfig lists the grpc client configuration parameters required to
// connect to Analyzer.
//
// If |EnableTLS| is false an insecure connection is used, and the remaining
// parameters except |URL| are ignored, otherwise TLS is used.
//
// |cc.CAFile| is optional. If non-empty it should specify the path to a file
// containing a PEM encoding of root certificates to use for TLS.
//
// |URL| specifies the url for the analyzer.
//
// |Timeout| specifies the time duration in seconds to terminate the client
// grpc connection to analyzer.
type GrpcClientConfig struct {
	EnableTLS bool
	CAFile    string
	Timeout   int
	URL       string
}

// GrpcAnalyzerTransport sends data to Analyzer specified by Grpc |clientConfig|
// using the |client| interface.
//
// |conn| handle is used for closing and re-establishing grpc connections when
// dispatcher toggles between send and wait modes.
type GrpcAnalyzerTransport struct {
	clientConfig *GrpcClientConfig
	conn         *grpc.ClientConn
	client       shufflerpb.AnalyzerClient
}

// NewGrpcAnalyzerTransport establishes a Grpc connection to the Analyzer
// specified by |clientConfig|, and returns a new |GrpcAnalyzerTransport|.
//
// Panics if |clientConfig| is nil or if the underlying grpc connection cannot
// be established.
func NewGrpcAnalyzerTransport(clientConfig *GrpcClientConfig) *GrpcAnalyzerTransport {
	conn := connect(clientConfig)

	return &GrpcAnalyzerTransport{
		clientConfig: clientConfig,
		conn:         conn,
		client:       shufflerpb.NewAnalyzerClient(conn),
	}
}

// connect returns a Grpc |ClientConn| handle after successfully establishing
// a connection to the analyzer endpoint using |cc| config parameters.
//
// If |cc.EnableTLS| is false an insecure connection is used, and the remaining
// parameters or ignored, otherwise TLS is used.
//
// |cc.CAFile| is optional. If non-empty it should specify the path to a file
// containing a PEM encoding of root certificates to use for TLS.
//
// Logs and crashes on any grpc failure, and panics if |cc| is not set.
func connect(cc *GrpcClientConfig) *grpc.ClientConn {
	if cc == nil {
		panic("Grpc client configuration is not set.")
	}

	glog.V(3).Infoln("Connecting to analyzer: %s", cc.URL)
	var opts []grpc.DialOption
	if cc.EnableTLS {
		var creds credentials.TransportCredentials
		if cc.CAFile != "" {
			var err error
			creds, err = credentials.NewClientTLSFromFile(cc.CAFile, "")
			if err != nil {
				glog.Fatalf("Failed to create TLS credentials %v", err)
			}
		} else {
			creds = credentials.NewClientTLSFromCert(nil, "")
		}
		opts = append(opts, grpc.WithTransportCredentials(creds))
	} else {
		opts = append(opts, grpc.WithInsecure())
	}
	opts = append(opts, grpc.WithBlock())
	opts = append(opts, grpc.WithTimeout(time.Second*time.Duration(cc.Timeout)))

	glog.V(3).Infoln("Dialing ", cc.URL, "...")
	conn, err := grpc.Dial(cc.URL, opts...)
	if err != nil {
		glog.Fatalf("Error in establishing connection to Analyzer [%v]: %v", cc.URL, err)
	}

	return conn
}

// close closes all the grpc underlying connections to Analyzer.
func (g *GrpcAnalyzerTransport) close() {
	if g == nil {
		panic("GrpcAnalyzerTransport is not set.")
	}

	if g.conn != nil {
		g.conn.Close()
		g.conn = nil
	}
}

// reconnect re-establishes the Grpc client connection to the Analyzer using the
// existing parameters from |g.clientConfig| and updates |g| accordingly.
func (g *GrpcAnalyzerTransport) reconnect() {
	if g == nil {
		panic("GrpcAnalyzerTransport is not set.")
	}

	if g.conn == nil {
		g.conn = connect(g.clientConfig)
		g.client = shufflerpb.NewAnalyzerClient(g.conn)
	}
}

// send forwards a given ObservationBatch to Analyzer using the AddObservations
// interface.
func (g *GrpcAnalyzerTransport) send(obBatch *shufflerpb.ObservationBatch) error {
	if g == nil {
		panic("GrpcAnalyzerTransport is not set.")
	}

	if obBatch == nil {
		return grpc.Errorf(codes.InvalidArgument, "ObservationBatch is not set.")
	}

	// Analyzer forwards a new context, so as to break the context correlation
	// between originating request and the shuffled request that is being
	// forwarded.
	glog.V(3).Infoln("Sending ObservationBatch ...")
	_, err := g.client.AddObservations(context.Background(), obBatch)
	if err != nil {
		return grpc.Errorf(codes.Internal, "AddObservations call failed with error: %v", err)
	}

	glog.V(3).Infoln("ObservationBatch dispatched successfully.")
	return nil
}

// Dispatcher stores and forwards encoder requests to |analyzer|s based on the
// type of |store|, |config|, |batchSize| and the |lastDispatchTime|.
type Dispatcher struct {
	store             storage.Store
	config            *shufflerpb.ShufflerConfig
	batchSize         int
	analyzerTransport AnalyzerTransport
	lastDispatchTime  time.Time
}

var dispatcherSingleton *Dispatcher

// Start function either routes the incoming request from Encoder to next
// Shuffler or to the Analyzer, if the dispatch criteria is met. If the
// dispatch criteria is not met, the incoming Observation is buffered locally
// for the next dispatch attempt.
func Start(config *shufflerpb.ShufflerConfig, store storage.Store, batchSize int, analyzerTransport AnalyzerTransport) {
	if store == nil {
		glog.Fatal("Invalid data store handle, exiting.")
	}

	if config == nil {
		glog.Fatal("Invalid server config, exiting.")
	}

	if analyzerTransport == nil {
		glog.Fatal("Invalid Analyzer client.")
	}

	if batchSize <= 0 {
		glog.Fatal("Invalid batch size.")
	}

	if dispatcherSingleton != nil {
		glog.Fatal("Start() must not be invoked twice, exiting.")
	}

	// invoke dispatcher
	dispatcherSingleton := &Dispatcher{
		store:             store,
		config:            config,
		batchSize:         batchSize,
		analyzerTransport: analyzerTransport,
		lastDispatchTime:  time.Time{},
	}
	dispatcherSingleton.Run()
}

// Run dispatches stored observations to the Analyzer per each
// ObservationMetadata key if threshold and dispatch frequency are met. If the
// criteria is not met, dispatcher goes back to wait mode until the next
// dispatch attempt.
//
// The underlying grpc connection to analyzer is closed when the dispatcher
// goes to sleep mode.
func (d *Dispatcher) Run() {
	for {
		waitTime := d.computeWaitTime(time.Now())
		if waitTime == 0 {
			waitTime = minWaitTimeInS
		}
		if waitTime > 0 {
			glog.V(3).Infoln("Close existing connection to Analyzer...")
			d.analyzerTransport.close()

			glog.V(3).Infof("Dispatcher going into sleep mode for [%ds]...", waitTime)
			time.Sleep(time.Duration(waitTime))

			glog.V(3).Infoln("Re-establish grpc connection to Analyzer before the next dispatch...")
			d.analyzerTransport.reconnect()
		}

		// lastDispatchTime is updated both at system startup and the elapsed time
		// between the dispatch attempts is greater than |frequencyInHours|.
		d.lastDispatchTime = time.Now()
		d.Dispatch()
	}
}

// Dispatch sends encoded observations to the Analyzer based on the following
// criteria:
// 1. The dispatch interval between attempts should be atleast
//    |frequency_in_hours| as specified in the Shuffler configuration.
// 2. If frequency is met, Shuffler sends |ObservationBatch| to the Analyzer for
//    each |ObservationMetadata| key if and only if:
//    - The batch contains atleast |threshold| number of Observations, and
//    - For each eligible batch, the Observations in that batch will be
//      dispatched to the Analyzer and deleted from the Shuffler, and
//    - For each batch whose Observations are not dispatched to the Analyzer
//      because the batch size is too small, the Shuffler will delete those
//      Observations from the batch whose age is at least |disposal_age_days|
//      specified in the configuration.
func (d *Dispatcher) Dispatch() {
	if d.store == nil {
		panic("Store handle is nil.")
	}

	if d.config == nil {
		panic("Shuffler config is nil.")
	}

	glog.V(3).Infoln("Start dispatching ...")
	keys, err := d.store.GetKeys()
	if err != nil {
		glog.Errorf("GetKeys() failed with error: %v", err)
		return
	}
	for _, key := range keys {
		// Fetch bucket size for each key
		bucketSize, err := d.store.GetNumObservations(key)
		glog.V(3).Infoln("Bucket size from store: [%d]", bucketSize)
		if err != nil {
			glog.Errorf("GetNumObservations() failed for key: %v with error: %v", key, err)
			continue
		}

		// Compare bucket size to the configured limit.
		if uint32(bucketSize) >= d.config.GetGlobalConfig().Threshold {
			err := d.dispatchBucket(key)
			if err != nil {
				glog.Errorf("dispatchBucket() failed for key: %v with error: %v", key, err)
				continue
			}
		} else {
			// If threshold policy is not met, loop through the messages and check
			// if any messages are in the queue for more than the allowed duration
			// |disposal_age_days|. If found, discard them, otherwise queue it back
			// in the store for the next dispatch event.
			err = d.deleteOldObservations(key, storage.GetDayIndexUtc(time.Now()), d.config.GetGlobalConfig().DisposalAgeDays)
			if err != nil {
				glog.Errorf("Error in filtering Observations for key [%v]: %v", key, err)
			}
		}
		time.Sleep(time.Duration(dispatchDelayInS))
	}
}

// dispatchBucket dispatches the ObservationBatch associated with |key| in
// chunks of size |batchSize| to Analyzer using grpc transport.
func (d *Dispatcher) dispatchBucket(key *shufflerpb.ObservationMetadata) error {
	if key == nil {
		panic("key is nil")
	}

	if d == nil {
		panic("dispatcher is nil")
	}

	// Retrieve shuffled bucket from store for the given |key|
	obVals, err := d.store.GetObservations(key)
	if err != nil {
		glog.Errorf("GetObservations() failed for key: %v with error: %v", key, err)
		return err
	}

	// Send the shuffled bucket to Analyzer in chunks. If the bucket is too
	// big, send it in multiple chunks of size |batchSize|.
	batchID := 0
	for i := 0; i < len(obVals); i += d.batchSize {
		batchID++
		glog.V(3).Infoln("Sending observations to Analyzer in chunks, batch [%d] in progress...", batchID)
		batchToSend := makeBatch(key, obVals, i, i+d.batchSize)
		sendErr := d.analyzerTransport.send(batchToSend)
		if sendErr != nil {
			// TODO(ukode): Add retry behaviour for 3 or more attempts or use
			// exponential backoff for errors relating to network issues.
			glog.Errorf("Error in transmitting data to Analyzer for key [%v]: %v", key, sendErr)
		}
		time.Sleep(time.Duration(dispatchDelayInS))
	}

	// After successful send, delete the observations from the local datastore.
	if err := d.store.DeleteValues(key, obVals); err != nil {
		glog.Errorf("Error in deleting dispatched observations from the store for key: %v", key)
		return err
	}

	return nil
}

// deleteOldObservations deletes the observations for a given |key| from the
// store if the age of the observation is greater than the configured value
// |disposalAgeInDays|.
func (d *Dispatcher) deleteOldObservations(key *shufflerpb.ObservationMetadata, currentDayIndex uint32, disposalAgeInDays uint32) error {
	if key == nil {
		panic("key is nil")
	}

	if d == nil {
		panic("dispatcher is nil")
	}

	obVals, err := d.store.GetObservations(key)
	if err != nil {
		glog.Errorf("GetObservation call failed for key: %v with error: %v", key, err)
		return nil
	} else if len(obVals) == 0 {
		return nil // nothing to be updated
	}

	var staleObVals []*shufflerpb.ObservationVal
	for _, obVal := range obVals {
		if currentDayIndex-obVal.ArrivalDayIndex > disposalAgeInDays {
			staleObVals = append(staleObVals, obVal)
		}
	}

	if err := d.store.DeleteValues(key, staleObVals); err != nil {
		return fmt.Errorf("Error [%v] in deleting old observations for metadata: %v", err, key)
	}
	return nil
}

// computeWaitTime returns |waitTimeInSecs| from the |currentTime| for the next
// dispatch event. If the Shuffler's dispatch interval is greater than the
// configured |frequency_in_hours| value, "0" is returned so dispatch happens
// immediately. Panics if dispatcher |d| is not set.
func (d *Dispatcher) computeWaitTime(currentTime time.Time) (waitTimeInSecs float64) {
	if d == nil {
		panic("Dispatcher is not set")
	}

	if d.config == nil {
		panic("Dispatcher's config is nil")
	}

	frequencyInHours := d.config.GetGlobalConfig().FrequencyInHours
	diff := currentTime.Sub(d.lastDispatchTime)
	if uint32(diff.Hours()) < frequencyInHours {
		waitTimeInSecs = diff.Seconds()
	}
	return
}

// makeBatch returns a new ObservationBatch for |key| consisting of a chunk
// of observations from |start| to |end| from the given |obVals| list.
func makeBatch(key *shufflerpb.ObservationMetadata, obVals []*shufflerpb.ObservationVal, start int, end int) *shufflerpb.ObservationBatch {
	var encryptedMessages []*shufflerpb.EncryptedMessage
	for i := start; i < end && i < len(obVals); i++ {
		encryptedMessages = append(encryptedMessages, obVals[i].EncryptedObservation)
	}

	return &shufflerpb.ObservationBatch{
		MetaData:             key,
		EncryptedObservation: encryptedMessages,
	}
}
