import java.io.DataInput;
import java.io.IOException;

public class ChdescSetFlags extends Opcode
{
	private final int chdesc, flags;
	
	public ChdescSetFlags(int chdesc, int flags)
	{
		this.chdesc = chdesc;
		this.flags = flags;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setFlags(flags);
	}
	
	public boolean hasEffect()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_FLAGS: chdesc = " + SystemState.hex(chdesc) + ", flags = " + Chdesc.renderFlags(flags);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_FLAGS, "KDB_CHDESC_SET_FLAGS", ChdescSetFlags.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("flags", 4);
		return factory;
	}
}
