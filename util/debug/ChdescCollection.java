import java.util.HashMap;
import java.util.Iterator;

public class ChdescCollection
{
	private final HashMap chdescs;
	private final String dupError;
	
	public ChdescCollection(String dupError)
	{
		chdescs = new HashMap();
		this.dupError = dupError;
	}
	
	public void add(Chdesc chdesc)
	{
		//Integer key = Integer.valueOf(chdesc.address);
		Integer key = new Integer(chdesc.address);
		if(chdescs.containsKey(key))
			throw new RuntimeException("Duplicate chdesc " + dupError + "!");
		chdescs.put(key, chdesc);
	}
	
	public Chdesc lookup(int chdesc)
	{
		//Integer key = Integer.valueOf(chdesc);
		Integer key = new Integer(chdesc);
		return (Chdesc) chdescs.get(key);
	}
	
	public Chdesc remove(int chdesc)
	{
		//Integer key = Integer.valueOf(chdesc);
		Integer key = new Integer(chdesc);
		return (Chdesc) chdescs.remove(key);
	}
	
	public int size()
	{
		return chdescs.size();
	}
	
	public Iterator iterator()
	{
		return new HashMapValueIterator(chdescs);
	}
}
