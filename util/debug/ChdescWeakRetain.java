import java.io.DataInput;
import java.io.IOException;

public class ChdescWeakRetain extends Opcode
{
	private final int chdesc, location;
	
	public ChdescWeakRetain(int chdesc, int location)
	{
		this.chdesc = chdesc;
		this.location = location;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.weakRetain(location);
	}
	
	public boolean hasEffect()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_WEAK_RETAIN: chdesc = " + SystemState.hex(chdesc) + ", location = " + SystemState.hex(location);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_WEAK_RETAIN, "KDB_CHDESC_WEAK_RETAIN", ChdescWeakRetain.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("location", 4);
		return factory;
	}
}
